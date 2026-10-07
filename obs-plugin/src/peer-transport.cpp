#include "peer-transport.hpp"

#include <obs-module.h>
#include <rtc/rtc.hpp>

#include <chrono>
#include <random>
#include <stdexcept>
#include <utility>

namespace tazzio {
namespace {

constexpr std::uint8_t video_payload_type = 102;
constexpr std::uint8_t audio_payload_type = 111;

std::uint32_t random_ssrc()
{
    std::random_device source;
    std::mt19937 engine(source());
    std::uniform_int_distribution<std::uint32_t> distribution(1, UINT32_MAX);
    return distribution(engine);
}

rtc::IceServer make_ice_server(const IceServerConfig &server)
{
    if (server.username.empty())
        return rtc::IceServer(server.url);

    const auto scheme_end = server.url.find(':');
    if (scheme_end == std::string::npos)
        throw std::invalid_argument("ICE server URL has no scheme");

    const auto scheme = server.url.substr(0, scheme_end);
    auto authority = server.url.substr(scheme_end + 1);
    while (!authority.empty() && authority.front() == '/')
        authority.erase(authority.begin());

    const auto query = authority.find('?');
    const auto transport = query == std::string::npos ? std::string{} : authority.substr(query + 1);
    if (query != std::string::npos)
        authority.resize(query);

    std::string hostname = authority;
    std::uint16_t port = scheme == "turns" ? 5349 : 3478;
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(':') == colon) {
        hostname = authority.substr(0, colon);
        port = static_cast<std::uint16_t>(std::stoul(authority.substr(colon + 1)));
    }

    auto relay = rtc::IceServer::RelayType::TurnUdp;
    if (scheme == "turns")
        relay = rtc::IceServer::RelayType::TurnTls;
    else if (transport.find("transport=tcp") != std::string::npos)
        relay = rtc::IceServer::RelayType::TurnTcp;

    return rtc::IceServer(hostname, port, server.username, server.credential, relay);
}

std::string state_name(rtc::PeerConnection::State state)
{
    switch (state) {
    case rtc::PeerConnection::State::New: return "new";
    case rtc::PeerConnection::State::Connecting: return "connecting";
    case rtc::PeerConnection::State::Connected: return "connected";
    case rtc::PeerConnection::State::Disconnected: return "disconnected";
    case rtc::PeerConnection::State::Failed: return "failed";
    case rtc::PeerConnection::State::Closed: return "closed";
    }
    return "unknown";
}

rtc::binary copy_binary(const std::byte *data, std::size_t size)
{
    return rtc::binary(data, data + size);
}

} // namespace

PeerTransport::PeerTransport() = default;

PeerTransport::~PeerTransport()
{
    close();
}

void PeerTransport::configure(std::vector<IceServerConfig> ice_servers)
{
    std::scoped_lock lock(mutex_);
    if (peer_)
        throw std::logic_error("Cannot reconfigure an active transport");
    ice_servers_ = std::move(ice_servers);
}

void PeerTransport::start(bool create_offer, bool receive_only)
{
    rtc::Configuration configuration;
    {
        std::scoped_lock lock(mutex_);
        if (peer_)
            throw std::logic_error("Transport is already started");
        for (const auto &server : ice_servers_)
            configuration.iceServers.emplace_back(make_ice_server(server));
        configuration.forceMediaTransport = true;
        peer_ = std::make_shared<rtc::PeerConnection>(configuration);
    }

    install_callbacks();
    if (!receive_only)
        create_outgoing_tracks();
    report_state("ready");
    if (create_offer)
        peer_->setLocalDescription(rtc::Description::Type::Offer);
}

void PeerTransport::install_callbacks()
{
    auto peer = peer_;
    peer->onLocalDescription([this](rtc::Description description) {
        DescriptionCallback callback;
        {
            std::scoped_lock lock(mutex_);
            callback = description_callback_;
        }
        if (callback)
            callback(description.typeString(), static_cast<std::string>(description));
    });
    peer->onLocalCandidate([this](rtc::Candidate candidate) {
        CandidateCallback callback;
        {
            std::scoped_lock lock(mutex_);
            callback = candidate_callback_;
        }
        if (callback)
            callback(candidate.candidate(), candidate.mid());
    });
    peer->onStateChange([this](rtc::PeerConnection::State state) { report_state(state_name(state)); });
    peer->onTrack([this](std::shared_ptr<rtc::Track> track) { attach_incoming_track(track); });
}

void PeerTransport::make_offer()
{
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::scoped_lock lock(mutex_);
        peer = peer_;
    }
    if (!peer)
        throw std::logic_error("Transport is not started");
    peer->setLocalDescription(rtc::Description::Type::Offer);
}

void PeerTransport::create_outgoing_tracks()
{
    const auto video_ssrc = random_ssrc();
    rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);
    video.addH264Codec(video_payload_type);
    video.addSSRC(video_ssrc, "tazzio-video", "tazzio-stream", "video");
    outgoing_video_ = peer_->addTrack(video);
    outgoing_video_->onOpen([] { blog(LOG_INFO, "[Tazzio iPhone Camera] outgoing video track open"); });
    outgoing_video_->onClosed([] { blog(LOG_INFO, "[Tazzio iPhone Camera] outgoing video track closed"); });
    auto video_config = std::make_shared<rtc::RtpPacketizationConfig>(
        video_ssrc, "tazzio-video", video_payload_type, rtc::H264RtpPacketizer::ClockRate);
    auto video_packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, video_config);
    video_packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(video_config));
    video_packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    outgoing_video_->setMediaHandler(video_packetizer);
    outgoing_video_->chainMediaHandler(
        std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
    outgoing_video_->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    outgoing_video_->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
        media_bytes_received_.fetch_add(data.size(), std::memory_order_relaxed);
        if (!received_video_frame_.exchange(true))
            blog(LOG_INFO, "[Tazzio iPhone Camera] first remote video frame received on negotiated track (%zu bytes)",
                 data.size());
        FrameCallback callback;
        {
            std::scoped_lock lock(mutex_);
            callback = video_callback_;
        }
        if (callback)
            callback({std::move(data), info.timestampSeconds ? info.timestampSeconds->count() : 0.0,
                      info.isKeyFrame});
    });

    const auto audio_ssrc = random_ssrc();
    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
    audio.addOpusCodec(audio_payload_type);
    audio.addSSRC(audio_ssrc, "tazzio-audio", "tazzio-stream", "audio");
    outgoing_audio_ = peer_->addTrack(audio);
    outgoing_audio_->onOpen([] { blog(LOG_INFO, "[Tazzio iPhone Camera] outgoing audio track open"); });
    outgoing_audio_->onClosed([] { blog(LOG_INFO, "[Tazzio iPhone Camera] outgoing audio track closed"); });
    auto audio_config = std::make_shared<rtc::RtpPacketizationConfig>(
        audio_ssrc, "tazzio-audio", audio_payload_type, rtc::OpusRtpPacketizer::DefaultClockRate);
    auto audio_packetizer = std::make_shared<rtc::OpusRtpPacketizer>(audio_config);
    audio_packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(audio_config));
    audio_packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    outgoing_audio_->setMediaHandler(audio_packetizer);
    outgoing_audio_->chainMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
    outgoing_audio_->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
    outgoing_audio_->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
        media_bytes_received_.fetch_add(data.size(), std::memory_order_relaxed);
        if (!received_audio_frame_.exchange(true))
            blog(LOG_INFO, "[Tazzio iPhone Camera] first remote audio frame received on negotiated track (%zu bytes)",
                 data.size());
        FrameCallback callback;
        {
            std::scoped_lock lock(mutex_);
            callback = audio_callback_;
        }
        if (callback)
            callback({std::move(data), info.timestampSeconds ? info.timestampSeconds->count() : 0.0, false});
    });
}

void PeerTransport::attach_incoming_track(const std::shared_ptr<rtc::Track> &track)
{
    const auto media_type = track->description().type();
    blog(LOG_INFO, "[Tazzio iPhone Camera] incoming WebRTC track: %s", media_type.c_str());
    if (media_type == "video") {
        {
            std::scoped_lock lock(mutex_);
            incoming_video_ = track;
        }
        track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence));
        track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
        track->onOpen([] { blog(LOG_INFO, "[Tazzio iPhone Camera] incoming video track open"); });
        track->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            media_bytes_received_.fetch_add(data.size(), std::memory_order_relaxed);
            if (!received_video_frame_.exchange(true))
                blog(LOG_INFO, "[Tazzio iPhone Camera] first remote video frame received (%zu bytes)", data.size());
            FrameCallback callback;
            {
                std::scoped_lock lock(mutex_);
                callback = video_callback_;
            }
            if (callback)
                callback({std::move(data), info.timestampSeconds ? info.timestampSeconds->count() : 0.0, info.isKeyFrame});
        });
    } else if (media_type == "audio") {
        {
            std::scoped_lock lock(mutex_);
            incoming_audio_ = track;
        }
        track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
        track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());
        track->onOpen([] { blog(LOG_INFO, "[Tazzio iPhone Camera] incoming audio track open"); });
        track->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
            media_bytes_received_.fetch_add(data.size(), std::memory_order_relaxed);
            if (!received_audio_frame_.exchange(true))
                blog(LOG_INFO, "[Tazzio iPhone Camera] first remote audio frame received (%zu bytes)", data.size());
            FrameCallback callback;
            {
                std::scoped_lock lock(mutex_);
                callback = audio_callback_;
            }
            if (callback)
                callback({std::move(data), info.timestampSeconds ? info.timestampSeconds->count() : 0.0, false});
        });
    }
}

void PeerTransport::set_remote_description(const std::string &type, const std::string &sdp)
{
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::scoped_lock lock(mutex_);
        peer = peer_;
    }
    if (!peer)
        throw std::logic_error("Transport is not started");
    peer->setRemoteDescription(rtc::Description(sdp, type));
}

void PeerTransport::add_remote_candidate(const std::string &candidate, const std::string &mid)
{
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::scoped_lock lock(mutex_);
        peer = peer_;
    }
    if (!peer)
        throw std::logic_error("Transport is not started");
    peer->addRemoteCandidate(rtc::Candidate(candidate, mid));
}

bool PeerTransport::send_h264(const std::byte *data, std::size_t size, double timestamp_seconds, bool keyframe)
{
    std::shared_ptr<rtc::Track> track;
    {
        std::scoped_lock lock(mutex_);
        track = outgoing_video_;
    }
    if (!track || !track->isOpen() || !data || size == 0)
        return false;
    rtc::FrameInfo info{std::chrono::duration<double>{timestamp_seconds}};
    info.isKeyFrame = keyframe;
    try {
        track->sendFrame(copy_binary(data, size), info);
        media_bytes_sent_.fetch_add(size, std::memory_order_relaxed);
        return true;
    } catch (const std::exception &error) {
        blog(LOG_WARNING, "[Tazzio iPhone Camera] video frame send failed: %s", error.what());
        return false;
    }
}

bool PeerTransport::send_opus(const std::byte *data, std::size_t size, double timestamp_seconds)
{
    std::shared_ptr<rtc::Track> track;
    {
        std::scoped_lock lock(mutex_);
        track = outgoing_audio_;
    }
    if (!track || !track->isOpen() || !data || size == 0)
        return false;
    rtc::FrameInfo info{std::chrono::duration<double>{timestamp_seconds}};
    try {
        track->sendFrame(copy_binary(data, size), info);
        media_bytes_sent_.fetch_add(size, std::memory_order_relaxed);
        return true;
    } catch (const std::exception &error) {
        blog(LOG_WARNING, "[Tazzio iPhone Camera] audio frame send failed: %s", error.what());
        return false;
    }
}

void PeerTransport::close()
{
    std::shared_ptr<rtc::PeerConnection> peer;
    {
        std::scoped_lock lock(mutex_);
        peer = std::move(peer_);
        if (outgoing_video_)
            outgoing_video_->resetCallbacks();
        if (outgoing_audio_)
            outgoing_audio_->resetCallbacks();
        if (incoming_video_)
            incoming_video_->resetCallbacks();
        if (incoming_audio_)
            incoming_audio_->resetCallbacks();
        outgoing_video_.reset();
        outgoing_audio_.reset();
        incoming_video_.reset();
        incoming_audio_.reset();
    }
    if (peer) {
        peer->resetCallbacks();
        peer->close();
    }
    received_video_frame_ = false;
    received_audio_frame_ = false;
    media_bytes_sent_ = 0;
    media_bytes_received_ = 0;
}

bool PeerTransport::connected() const
{
    std::scoped_lock lock(mutex_);
    return peer_ && peer_->state() == rtc::PeerConnection::State::Connected;
}

TransportStats PeerTransport::stats() const
{
    std::scoped_lock lock(mutex_);
    TransportStats result;
    if (!peer_)
        return result;
    result.bytes_sent = media_bytes_sent_.load(std::memory_order_relaxed);
    result.bytes_received = media_bytes_received_.load(std::memory_order_relaxed);
    result.video_track_open = outgoing_video_ && outgoing_video_->isOpen();
    result.audio_track_open = outgoing_audio_ && outgoing_audio_->isOpen();
    if (const auto rtt = peer_->rtt())
        result.round_trip_ms = static_cast<int>(rtt->count());
    rtc::Candidate local;
    rtc::Candidate remote;
    if (peer_->getSelectedCandidatePair(&local, &remote)) {
        result.local_candidate = local.candidate();
        result.remote_candidate = remote.candidate();
    }
    return result;
}

void PeerTransport::on_local_description(DescriptionCallback callback)
{
    std::scoped_lock lock(mutex_);
    description_callback_ = std::move(callback);
}

void PeerTransport::on_local_candidate(CandidateCallback callback)
{
    std::scoped_lock lock(mutex_);
    candidate_callback_ = std::move(callback);
}

void PeerTransport::on_state(StateCallback callback)
{
    std::scoped_lock lock(mutex_);
    state_callback_ = std::move(callback);
}

void PeerTransport::on_video(FrameCallback callback)
{
    std::scoped_lock lock(mutex_);
    video_callback_ = std::move(callback);
}

void PeerTransport::on_audio(FrameCallback callback)
{
    std::scoped_lock lock(mutex_);
    audio_callback_ = std::move(callback);
}

void PeerTransport::report_state(const std::string &state) const
{
    StateCallback callback;
    {
        std::scoped_lock lock(mutex_);
        callback = state_callback_;
    }
    if (callback)
        callback(state);
}

} // namespace tazzio
