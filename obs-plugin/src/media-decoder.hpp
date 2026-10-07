#pragma once

#include "peer-transport.hpp"

#include <cstdint>
#include <memory>

struct obs_source;
typedef struct obs_source obs_source_t;

namespace tazzio {

enum class DecoderChannel {
    CoStream = 0,
    IPhoneCamera = 1,
};

class MediaDecoder final {
public:
    explicit MediaDecoder(obs_source_t *source, DecoderChannel channel = DecoderChannel::CoStream);
    ~MediaDecoder();

    MediaDecoder(const MediaDecoder &) = delete;
    MediaDecoder &operator=(const MediaDecoder &) = delete;

    void decode_h264(EncodedFrame frame);
    void decode_opus(EncodedFrame frame);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct RemoteDecodeStats {
    std::uint64_t video_frames{};
    std::uint64_t audio_frames{};
    std::uint64_t video_errors{};
    std::uint64_t audio_errors{};
    bool decoder_present{};
};

void register_remote_decoder(MediaDecoder *decoder);
void unregister_remote_decoder(MediaDecoder *decoder);
void route_remote_video(EncodedFrame frame);
void route_remote_audio(EncodedFrame frame);
RemoteDecodeStats remote_decode_stats();
void register_camera_decoder(MediaDecoder *decoder);
void unregister_camera_decoder(MediaDecoder *decoder);
void route_camera_video(EncodedFrame frame);
void route_camera_audio(EncodedFrame frame);
RemoteDecodeStats camera_decode_stats();

} // namespace tazzio
