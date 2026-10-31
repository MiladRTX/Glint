#pragma once
#include "audio.h"
#include "decoder.h"
#include "media.h"
#include "recorder.h"
#include "glint/payload_client.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace glint {

// The live session: connect to the payload, decode and pace the picture,
// play the sound, record, take screenshots.
class App {
public:
    enum class Latency { Lowest, Balanced, Smooth };

    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void start(const std::string& url);
    void stop();
    bool running() const;
    bool connected() const;
    std::string url() const;
    std::string error() const;
    StreamConfig config() const;
    StreamStatus status() const;
    bool post(const std::string& path, std::string& error);

    void set_frame_notify(std::function<void()> notify);
    std::shared_ptr<const Picture> take_frame(uint64_t& last_seen);
    uint64_t frame_count() const;
    void received_rate(float& fps, float& kbps) const;
    std::vector<float> recent_lateness(size_t count) const;
    uint64_t late_frames() const;
    float video_hold_ms() const;

    void set_latency(Latency latency);
    Latency latency() const;
    void set_video_sync(bool on);
    bool video_sync() const;
    void set_av_offset_ms(int ms);
    int av_offset_ms() const;

    void set_gpu_decode(bool on);
    bool gpu_decode() const;
    void set_gpu_convert(bool on);
    bool gpu_convert() const;
    std::string decoder_description() const;

    AudioPlayer& audio();

    std::string media_dir() const;
    void set_media_dir(const std::string& dir);
    bool take_screenshot(std::string& saved_path, std::string& error);
    bool start_local_recording(std::string& error);
    void stop_local_recording();
    RecorderInfo recording_info() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}
