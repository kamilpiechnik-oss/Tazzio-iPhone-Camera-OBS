#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rtc {
class PeerConnection;
class Track;
}

namespace tazzio {

struct IceServerConfig {
    std::string url;
    std::string username;
    std::string credential;
};

struct EncodedFrame {
    std::vector<std::byte> data;
    double timestamp_seconds{};
    bool keyframe{};
};

struct TransportStats {
    std::size_t bytes_sent{};
    std::size_t bytes_received{};
    bool video_track_open{};
    bool audio_track_open{};
    int round_trip_ms{-1};
    std::string local_candidate;
    std::string remote_candidate;
};

class PeerTransport final {
public:
    using DescriptionCallback = std::function<void(const std::string &type, const std::string &sdp)>;
    using CandidateCallback = std::function<void(const std::string &candidate, const std::string &mid)>;
    using StateCallback = std::function<void(const std::string &state)>;
    using FrameCallback = std::function<void(EncodedFrame frame)>;

    PeerTransport();
    ~PeerTransport();

    PeerTransport(const PeerTransport &) = delete;
    PeerTransport &operator=(const PeerTransport &) = delete;

    void configure(std::vector<IceServerConfig> ice_servers);
    void start(bool create_offer, bool receive_only = false);
    void make_offer();
    void set_remote_description(const std::string &type, const std::string &sdp);
    void add_remote_candidate(const std::string &candidate, const std::string &mid);
    bool send_h264(const std::byte *data, std::size_t size, double timestamp_seconds, bool keyframe);
    bool send_opus(const std::byte *data, std::size_t size, double timestamp_seconds);
    void close();

    [[nodiscard]] bool connected() const;
    [[nodiscard]] TransportStats stats() const;

    void on_local_description(DescriptionCallback callback);
    void on_local_candidate(CandidateCallback callback);
    void on_state(StateCallback callback);
    void on_video(FrameCallback callback);
    void on_audio(FrameCallback callback);

private:
    void install_callbacks();
    void create_outgoing_tracks();
    void attach_incoming_track(const std::shared_ptr<rtc::Track> &track);
    void report_state(const std::string &state) const;

    mutable std::mutex mutex_;
    std::vector<IceServerConfig> ice_servers_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::shared_ptr<rtc::Track> outgoing_video_;
    std::shared_ptr<rtc::Track> outgoing_audio_;
    std::shared_ptr<rtc::Track> incoming_video_;
    std::shared_ptr<rtc::Track> incoming_audio_;
    DescriptionCallback description_callback_;
    CandidateCallback candidate_callback_;
    StateCallback state_callback_;
    FrameCallback video_callback_;
    FrameCallback audio_callback_;
    std::atomic_bool received_video_frame_{};
    std::atomic_bool received_audio_frame_{};
    std::atomic_size_t media_bytes_sent_{};
    std::atomic_size_t media_bytes_received_{};
};

} // namespace tazzio
