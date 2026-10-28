#pragma once

#include "audio.h"
#include "broadcast.h"
#include "decoder.h"
#include "media.h"
#include "glint/payload_client.h"
#include "recorder.h"

#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace glint {

class App {
public:
    App() = default;
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    void start(const std::string& url);
    void stop();
    bool running() const { return running_.load(); }
    bool connected() const { return connected_.load(); }

    bool post(const std::string& path, std::string& error);

    StreamConfig config() const;
    StreamStatus status() const;
    std::string error() const;
    std::string url() const;

    // Returns the latest decoded frame (RGBA) when its sequence is newer than
    // last_seen. The frame buffer is shared, no copy.
    std::shared_ptr<const Picture> take_frame(uint64_t& last_seen);

    // Decoding: D3D11VA on the GPU (falls back to the CPU on failure) and where
    // YUV becomes RGB (GPU = Vulkan compute in the UI, CPU = swscale).
    void set_gpu_decode(bool on) { gpu_decode_.store(on); }
    bool gpu_decode() const { return gpu_decode_.load(); }
    void set_gpu_convert(bool on) { gpu_convert_.store(on); }
    bool gpu_convert() const { return gpu_convert_.load(); }
    std::string decoder_description() const;

    // Called from the decode thread after each new frame (the UI uses it to
    // wake its render loop). Set before start().
    void set_frame_notify(std::function<void()> notify) { frame_notify_ = std::move(notify); }
    uint64_t frame_count() const { return frames_.load(); }

    AudioPlayer& audio() { return audio_; }

    // Local recording / screenshots / media folder ---------------------------
    bool start_local_recording(std::string& error);
    void stop_local_recording();
    RecorderInfo recording_info() const { return recorder_.info(); }

    // Live broadcast -----------------------------------------------------------
    bool start_broadcast(const std::vector<BroadcastTarget>& targets, const BroadcastQuality& quality,
                         std::string& error);
    void stop_broadcast(const std::string& reason = std::string());
    BroadcastStatus broadcast_status() const { return broadcaster_.status(); }
    bool take_screenshot(std::string& saved_path, std::string& error);

    // A/V sync: hold decoded video by the audio playout latency (+ offset).
    void set_video_sync(bool on) { video_sync_.store(on); }
    bool video_sync() const { return video_sync_.load(); }

    // Latency. Lowest: show each frame as it arrives (no hold), small sound
    // buffer. Balanced: pace frames by the console's timestamps, holding only
    // what covers 99 % of the measured jitter (tens of ms), small sound buffer.
    // Smooth: pace frames through a larger jitter buffer and keep the picture
    // in sync with the sound.
    enum class Latency { Lowest, Balanced, Smooth };
    void set_latency(Latency latency);
    Latency latency() const { return (Latency)latency_.load(); }
    float video_jitter_ms() const { return video_jitter_ms_.load(); }
    // Frames that arrived after their paced slot (Balanced/Smooth): hitches.
    uint64_t late_frames() const { return late_frames_.load(); }
    // How late each of the last `count` frames arrived (ms above the best
    // case seen), oldest first. Drawn as the frame strip.
    std::vector<float> recent_lateness(size_t count) const;
    // Video received over the last 2 s. The console's status fps/kbps are an
    // average since its encoder started and overcount after an encoder reopen.
    void received_rate(float& fps, float& kbps) const;
    void set_av_offset_ms(int ms) { av_offset_ms_.store(ms < -200 ? -200 : ms > 200 ? 200 : ms); }
    int av_offset_ms() const { return av_offset_ms_.load(); }
    float video_hold_ms() const { return video_hold_ms_.load(); }
    std::string media_dir() const;
    void set_media_dir(const std::string& dir);

private:
    void worker();
    void status_worker();
    double video_hold_target_ms() const;

    std::string url_;
    std::thread worker_;
    std::thread status_thread_;
    std::atomic_bool running_{false};
    std::atomic_bool stop_{false};
    std::atomic_bool connected_{false};
    std::atomic<uint64_t> frames_{0};
    LocalRecorder recorder_;   // declared before audio_: audio threads die first
    Broadcaster broadcaster_;  // likewise
    AudioPlayer audio_;
    std::atomic<int> rec_width_{0};
    std::atomic<int> rec_height_{0};
    std::atomic<bool> video_sync_{true};
    std::atomic<int> latency_{(int)Latency::Lowest};
    std::atomic<float> video_jitter_ms_{0.0f};       // decaying peak (Smooth)
    std::atomic<float> video_jitter_p99_ms_{0.0f};   // last 10 s, 99th percentile (Balanced)
    std::atomic<uint64_t> late_frames_{0};
    mutable std::mutex lateness_mutex_;
    float lateness_ring_[240] = {};
    size_t lateness_next_ = 0, lateness_count_ = 0;
    std::atomic<float> rx_fps_{0.0f};
    std::atomic<float> rx_kbps_{0.0f};
    std::atomic<int64_t> rx_last_ms_{0};   // steady clock, last frame received
    std::atomic<int> av_offset_ms_{0};
    std::atomic<float> video_hold_ms_{0.0f};
    std::string media_dir_ = default_media_dir();

    mutable std::mutex mutex_;
    StreamConfig config_;
    StreamStatus status_;
    std::string error_;
    std::shared_ptr<Picture> frame_;
    std::atomic<bool> gpu_decode_{true};
    std::atomic<bool> gpu_convert_{true};
    std::string decoder_desc_;
    std::function<void()> frame_notify_;
    int frame_width_ = 0;
    int frame_height_ = 0;
    uint64_t frame_seq_ = 0;
};

} // namespace glint
