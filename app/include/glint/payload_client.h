#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace glint {

struct StreamConfig {
    std::string platform;
    int width = 0;
    int height = 0;
    int fps = 0;
    std::string codec;
    std::string sps;
    std::string pps;
    bool has_audio = false;
    std::string audio_codec;
    int audio_channels = 0;
    int audio_rate = 0;
    int audio_bits = 0;
    std::vector<int> resolutions;
    int requested_height = 0;
    bool profile_changing = false;
    bool profile_error = false;
};

struct StreamStatus {
    std::string backend;
    int width = 0;
    int height = 0;
    unsigned long long frames = 0;
    unsigned long long bytes = 0;
    double fps = 0.0;
    double kbps = 0.0;
    int clients = 0;
    bool recording = false;
    std::string recording_name;
    unsigned long long recording_frames = 0;
    unsigned long long recording_bytes = 0;
    double recording_seconds = 0.0;
    bool recording_error = false;
    unsigned long long audio_chunks = 0;
    unsigned long long audio_bytes = 0;
    double audio_kbps = 0.0;
    std::string audio_codec;
};

// Port the Glint payload listens on.
constexpr int kPayloadPort = 7231;

class PayloadClient {
public:
    // base_url like "http://<ps5-ip>:7231" (no trailing slash)
    explicit PayloadClient(std::string base_url);

    bool fetch_config(StreamConfig& config, std::string& error);
    bool fetch_status(StreamStatus& status, std::string& error);
    bool fetch_bytes(const std::string& path, std::vector<uint8_t>& out,
                     std::string& error, long timeout_ms = 5000);
    bool post(const std::string& path, std::string* body, std::string& error);

    using FrameCallback =
        std::function<bool(const uint8_t* annexb, size_t len, uint32_t pts)>;
    using AudioCallback =
        std::function<bool(const uint8_t* pcm, size_t len, uint32_t pts)>;

    bool stream_frames(const FrameCallback& callback, std::atomic_bool& stop,
                       std::string& error, int idle_timeout_ms = 9000);
    bool stream_audio(const AudioCallback& callback, std::atomic_bool& stop,
                      std::string& error, int idle_timeout_ms = 9000);

    const std::string& base_url() const { return base_; }

private:
    std::string base_;
};

std::string json_string(const std::string& document, const std::string& key);
long long json_int(const std::string& document, const std::string& key, long long fallback = 0);
double json_double(const std::string& document, const std::string& key, double fallback = 0.0);
bool json_bool(const std::string& document, const std::string& key, bool fallback = false);
std::vector<int> json_int_array(const std::string& document, const std::string& key);

} // namespace glint
