#include "media-decoder.hpp"

#include <obs-module.h>
#include <util/platform.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace tazzio {
namespace {

std::mutex router_mutex;
std::array<MediaDecoder *, 2> active_decoders{};
std::array<std::atomic_uint64_t, 2> decoded_video_frames{};
std::array<std::atomic_uint64_t, 2> decoded_audio_frames{};
std::array<std::atomic_uint64_t, 2> video_decode_errors{};
std::array<std::atomic_uint64_t, 2> audio_decode_errors{};

constexpr std::size_t channel_index(DecoderChannel channel)
{
    return static_cast<std::size_t>(channel);
}

speaker_layout speaker_layout_for(int channels)
{
    switch (channels) {
    case 1: return SPEAKERS_MONO;
    case 2: return SPEAKERS_STEREO;
    case 3: return SPEAKERS_2POINT1;
    case 4: return SPEAKERS_4POINT0;
    case 6: return SPEAKERS_5POINT1;
    case 8: return SPEAKERS_7POINT1;
    default: return SPEAKERS_UNKNOWN;
    }
}

audio_format audio_format_for(AVSampleFormat format)
{
    switch (format) {
    case AV_SAMPLE_FMT_U8: return AUDIO_FORMAT_U8BIT;
    case AV_SAMPLE_FMT_S16: return AUDIO_FORMAT_16BIT;
    case AV_SAMPLE_FMT_S32: return AUDIO_FORMAT_32BIT;
    case AV_SAMPLE_FMT_FLT: return AUDIO_FORMAT_FLOAT;
    case AV_SAMPLE_FMT_U8P: return AUDIO_FORMAT_U8BIT_PLANAR;
    case AV_SAMPLE_FMT_S16P: return AUDIO_FORMAT_16BIT_PLANAR;
    case AV_SAMPLE_FMT_S32P: return AUDIO_FORMAT_32BIT_PLANAR;
    case AV_SAMPLE_FMT_FLTP: return AUDIO_FORMAT_FLOAT_PLANAR;
    default: return AUDIO_FORMAT_UNKNOWN;
    }
}

video_colorspace video_colorspace_for(AVColorSpace colorspace)
{
    switch (colorspace) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        return VIDEO_CS_601;
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
    default:
        // Webcam H.264 streams commonly omit this metadata. HD video must
        // default to BT.709 instead of leaving OBS' color matrix zeroed.
        return VIDEO_CS_709;
    }
}

} // namespace

class MediaDecoder::Impl {
public:
    explicit Impl(obs_source_t *source, DecoderChannel channel) : source(source), channel(channel)
    {
        const auto *h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
        const auto *opus = avcodec_find_decoder(AV_CODEC_ID_OPUS);
        if (h264) {
            video = avcodec_alloc_context3(h264);
            video->flags |= AV_CODEC_FLAG_LOW_DELAY;
            video->thread_count = 1;
            if (avcodec_open2(video, h264, nullptr) < 0) {
                avcodec_free_context(&video);
                blog(LOG_ERROR, "[Tazzio iPhone Camera] unable to open H.264 decoder");
            }
        }
        else {
            blog(LOG_ERROR, "[Tazzio iPhone Camera] H.264 decoder is unavailable");
        }
        if (opus) {
            audio = avcodec_alloc_context3(opus);
            audio->sample_rate = 48000;
            av_channel_layout_default(&audio->ch_layout, 2);
            if (avcodec_open2(audio, opus, nullptr) < 0) {
                avcodec_free_context(&audio);
                blog(LOG_ERROR, "[Tazzio iPhone Camera] unable to open Opus decoder");
            }
        }
        else {
            blog(LOG_ERROR, "[Tazzio iPhone Camera] Opus decoder is unavailable");
        }
        frame = av_frame_alloc();
    }

    ~Impl()
    {
        if (sws)
            sws_freeContext(sws);
        av_frame_free(&frame);
        avcodec_free_context(&video);
        avcodec_free_context(&audio);
    }

    void decode_video(EncodedFrame encoded)
    {
        std::scoped_lock lock(mutex);
        if (!video || !frame || encoded.data.empty())
            return;
        std::vector<std::uint8_t> packet_data(encoded.data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(packet_data.data(), encoded.data.data(), encoded.data.size());
        AVPacket packet{};
        packet.data = packet_data.data();
        packet.size = static_cast<int>(encoded.data.size());
        const auto send_result = avcodec_send_packet(video, &packet);
        if (send_result < 0) {
            const auto errors = ++video_decode_errors[channel_index(channel)];
            if (errors <= 3)
                blog(LOG_WARNING, "[Tazzio iPhone Camera] H.264 decoder rejected a frame: %d", send_result);
            return;
        }
        while (avcodec_receive_frame(video, frame) == 0) {
            output_video(encoded.timestamp_seconds);
            const auto count = ++decoded_video_frames[channel_index(channel)];
            if (count == 1)
                blog(LOG_INFO, "[Tazzio iPhone Camera] first H.264 frame decoded: %dx%d", frame->width, frame->height);
        }
    }

    void decode_audio(EncodedFrame encoded)
    {
        std::scoped_lock lock(mutex);
        if (!audio || !frame || encoded.data.empty())
            return;
        std::vector<std::uint8_t> packet_data(encoded.data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        std::memcpy(packet_data.data(), encoded.data.data(), encoded.data.size());
        AVPacket packet{};
        packet.data = packet_data.data();
        packet.size = static_cast<int>(encoded.data.size());
        const auto send_result = avcodec_send_packet(audio, &packet);
        if (send_result < 0) {
            const auto errors = ++audio_decode_errors[channel_index(channel)];
            if (errors <= 3)
                blog(LOG_WARNING, "[Tazzio iPhone Camera] Opus decoder rejected a frame: %d", send_result);
            return;
        }
        while (avcodec_receive_frame(audio, frame) == 0) {
            output_audio(encoded.timestamp_seconds);
            const auto count = ++decoded_audio_frames[channel_index(channel)];
            if (count == 1)
                blog(LOG_INFO, "[Tazzio iPhone Camera] first Opus frame decoded");
        }
    }

private:
    std::uint64_t map_timestamp(double timestamp, bool video_frame)
    {
        auto &initialized = video_frame ? video_clock_initialized : audio_clock_initialized;
        auto &media_base = video_frame ? video_timestamp_base : audio_timestamp_base;
        auto &obs_base = video_frame ? video_obs_base : audio_obs_base;
        const auto now = os_gettime_ns();
        if (!std::isfinite(timestamp))
            return now;
        if (!initialized || timestamp < media_base - 0.1 || timestamp - media_base > 3600.0) {
            initialized = true;
            media_base = timestamp;
            obs_base = now;
            return now;
        }
        return obs_base + static_cast<std::uint64_t>((timestamp - media_base) * 1'000'000'000.0);
    }

    void output_video(double timestamp)
    {
        obs_source_frame output{};
        output.width = static_cast<std::uint32_t>(frame->width);
        output.height = static_cast<std::uint32_t>(frame->height);
        output.timestamp = map_timestamp(timestamp, true);

        if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
            output.format = VIDEO_FORMAT_I420;
            for (int plane = 0; plane < 3; ++plane) {
                output.data[plane] = frame->data[plane];
                output.linesize[plane] = static_cast<std::uint32_t>(frame->linesize[plane]);
            }
        } else if (frame->format == AV_PIX_FMT_NV12) {
            output.format = VIDEO_FORMAT_NV12;
            for (int plane = 0; plane < 2; ++plane) {
                output.data[plane] = frame->data[plane];
                output.linesize[plane] = static_cast<std::uint32_t>(frame->linesize[plane]);
            }
        } else {
            const auto buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frame->width, frame->height, 1);
            converted.resize(static_cast<std::size_t>(buffer_size));
            std::array<std::uint8_t *, 4> planes{};
            std::array<int, 4> lines{};
            av_image_fill_arrays(planes.data(), lines.data(), converted.data(), AV_PIX_FMT_YUV420P,
                                 frame->width, frame->height, 1);
            sws = sws_getCachedContext(sws, frame->width, frame->height,
                                       static_cast<AVPixelFormat>(frame->format), frame->width, frame->height,
                                       AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws)
                return;
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height, planes.data(), lines.data());
            output.format = VIDEO_FORMAT_I420;
            for (int plane = 0; plane < 3; ++plane) {
                output.data[plane] = planes[plane];
                output.linesize[plane] = static_cast<std::uint32_t>(lines[plane]);
            }
        }

        const auto colorspace = video_colorspace_for(frame->colorspace);
        const auto range = frame->color_range == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
        output.full_range = range == VIDEO_RANGE_FULL;
        if (!video_format_get_parameters_for_format(colorspace, range, output.format,
                                                    output.color_matrix, output.color_range_min,
                                                    output.color_range_max)) {
            ++video_decode_errors[channel_index(channel)];
            blog(LOG_WARNING, "[Tazzio iPhone Camera] unable to prepare OBS color parameters for decoded video");
            return;
        }
        obs_source_output_video(source, &output);
    }

    void output_audio(double timestamp)
    {
        const auto format = audio_format_for(static_cast<AVSampleFormat>(frame->format));
        const auto speakers = speaker_layout_for(frame->ch_layout.nb_channels);
        if (format == AUDIO_FORMAT_UNKNOWN || speakers == SPEAKERS_UNKNOWN)
            return;
        obs_source_audio output{};
        output.frames = static_cast<std::uint32_t>(frame->nb_samples);
        output.samples_per_sec = static_cast<std::uint32_t>(frame->sample_rate > 0 ? frame->sample_rate : 48000);
        output.format = format;
        output.speakers = speakers;
        output.timestamp = map_timestamp(timestamp, false);
        const auto planes = av_sample_fmt_is_planar(static_cast<AVSampleFormat>(frame->format))
                                ? std::min(frame->ch_layout.nb_channels, MAX_AV_PLANES)
                                : 1;
        for (int plane = 0; plane < planes; ++plane)
            output.data[plane] = frame->extended_data[plane];
        obs_source_output_audio(source, &output);
    }

    obs_source_t *source{};
    DecoderChannel channel{DecoderChannel::CoStream};
    AVCodecContext *video{};
    AVCodecContext *audio{};
    AVFrame *frame{};
    SwsContext *sws{};
    std::vector<std::uint8_t> converted;
    std::mutex mutex;
    bool video_clock_initialized{};
    bool audio_clock_initialized{};
    double video_timestamp_base{};
    double audio_timestamp_base{};
    std::uint64_t video_obs_base{};
    std::uint64_t audio_obs_base{};
};

MediaDecoder::MediaDecoder(obs_source_t *source, DecoderChannel channel) : impl_(std::make_unique<Impl>(source, channel)) {}
MediaDecoder::~MediaDecoder() = default;
void MediaDecoder::decode_h264(EncodedFrame frame) { impl_->decode_video(std::move(frame)); }
void MediaDecoder::decode_opus(EncodedFrame frame) { impl_->decode_audio(std::move(frame)); }

void register_remote_decoder(MediaDecoder *decoder)
{
    std::scoped_lock lock(router_mutex);
    const auto index = channel_index(DecoderChannel::CoStream);
    active_decoders[index] = decoder;
    decoded_video_frames[index] = 0;
    decoded_audio_frames[index] = 0;
    video_decode_errors[index] = 0;
    audio_decode_errors[index] = 0;
}

void unregister_remote_decoder(MediaDecoder *decoder)
{
    std::scoped_lock lock(router_mutex);
    auto &active = active_decoders[channel_index(DecoderChannel::CoStream)];
    if (active == decoder)
        active = nullptr;
}

void route_remote_video(EncodedFrame frame)
{
    std::scoped_lock lock(router_mutex);
    auto *active = active_decoders[channel_index(DecoderChannel::CoStream)];
    if (active)
        active->decode_h264(std::move(frame));
}

void route_remote_audio(EncodedFrame frame)
{
    std::scoped_lock lock(router_mutex);
    auto *active = active_decoders[channel_index(DecoderChannel::CoStream)];
    if (active)
        active->decode_opus(std::move(frame));
}

RemoteDecodeStats remote_decode_stats()
{
    std::scoped_lock lock(router_mutex);
    const auto index = channel_index(DecoderChannel::CoStream);
    return {decoded_video_frames[index].load(), decoded_audio_frames[index].load(),
            video_decode_errors[index].load(), audio_decode_errors[index].load(), active_decoders[index] != nullptr};
}

void register_camera_decoder(MediaDecoder *decoder)
{
    std::scoped_lock lock(router_mutex);
    const auto index = channel_index(DecoderChannel::IPhoneCamera);
    active_decoders[index] = decoder;
    decoded_video_frames[index] = 0;
    decoded_audio_frames[index] = 0;
    video_decode_errors[index] = 0;
    audio_decode_errors[index] = 0;
}

void unregister_camera_decoder(MediaDecoder *decoder)
{
    std::scoped_lock lock(router_mutex);
    auto &active = active_decoders[channel_index(DecoderChannel::IPhoneCamera)];
    if (active == decoder)
        active = nullptr;
}

void route_camera_video(EncodedFrame frame)
{
    std::scoped_lock lock(router_mutex);
    auto *active = active_decoders[channel_index(DecoderChannel::IPhoneCamera)];
    if (active)
        active->decode_h264(std::move(frame));
}

void route_camera_audio(EncodedFrame frame)
{
    std::scoped_lock lock(router_mutex);
    auto *active = active_decoders[channel_index(DecoderChannel::IPhoneCamera)];
    if (active)
        active->decode_opus(std::move(frame));
}

RemoteDecodeStats camera_decode_stats()
{
    std::scoped_lock lock(router_mutex);
    const auto index = channel_index(DecoderChannel::IPhoneCamera);
    return {decoded_video_frames[index].load(), decoded_audio_frames[index].load(),
            video_decode_errors[index].load(), audio_decode_errors[index].load(), active_decoders[index] != nullptr};
}

} // namespace tazzio
