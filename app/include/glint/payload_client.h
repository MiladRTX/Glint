#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace glint {

// GET /config.json
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

// GET /status.json
struct StreamStatus {
    std::string backend;
    int width = 0;
    int height = 0;
    unsigned long long frames = 0;
    unsigned long long bytes = 0;
    double fps = 0.0;
    double kbps = 0.0;
    int clients = 0;
    int viewers = 0;
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

constexpr int kPayloadPort = 7231;

// The exact error a stream call returns when the console refused it with 409
// because another viewer is watching.
extern const char* const kTakenOver;

// HTTP client for the payload. One instance per thread.
class PayloadClient {
public:
    explicit PayloadClient(std::string base_url);

    const std::string& base_url() const { return base_; }
    // Optional: a flag that aborts any request in progress when it becomes true.
    void set_cancel(const std::atomic_bool* cancel) { cancel_ = cancel; }

    bool fetch_config(StreamConfig& config, std::string& error);
    bool fetch_status(StreamStatus& status, std::string& error);
    bool post(const std::string& path, std::string* body, std::string& error);

    using FrameCallback = std::function<bool(const uint8_t* annexb, size_t len, uint32_t pts)>;
    using AudioCallback = std::function<bool(const uint8_t* pcm, size_t len, uint32_t pts)>;

    bool stream_frames(const FrameCallback& callback, std::atomic_bool& stop,
                       std::string& error, int idle_timeout_ms = 9000);
    bool stream_audio(const AudioCallback& callback, std::atomic_bool& stop,
                      std::string& error, int idle_timeout_ms = 9000);

private:
    bool fetch_json(const std::string& path, std::string& body, std::string& error);
    bool stream_records(const std::string& path, size_t max_len,
                        const std::function<bool(const uint8_t*, size_t, uint32_t)>& callback,
                        std::atomic_bool& stop, std::string& error, int idle_timeout_ms);

    std::string base_;
    const std::atomic_bool* cancel_ = nullptr;
};

}
