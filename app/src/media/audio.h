#pragma once
#include "glint/payload_client.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace glint {

struct AudioStats {
    bool device_open = false;
    bool streaming = false;
    bool primed = false;
    std::string state;
    std::string device;
    std::string format;
    int rate = 0;

    uint64_t packets = 0;
    uint64_t frames_in = 0;
    uint64_t frames_out = 0;
    uint64_t callbacks = 0;
    uint64_t drop_events = 0;
    uint64_t dropped_frames = 0;
    uint64_t underruns = 0;
    uint64_t underrun_frames = 0;
    uint64_t underruns_after_warmup = 0;
    bool warmed_up = false;
    uint64_t nonfinite_in = 0;
    uint64_t nonfinite_out = 0;
    uint64_t clipped_samples = 0;
    uint64_t reconnects = 0;

    double max_packet_gap_ms = 0;
    uint64_t packet_gaps_50ms = 0;
    uint64_t packet_gaps_100ms = 0;
    uint64_t packet_gaps_200ms = 0;
    uint64_t packet_gaps_400ms = 0;
    double max_callback_gap_ms = 0;
    uint64_t callback_gaps_40ms = 0;

    double buffered_ms = 0;
    double target_ms = 0;
    bool auto_delay = false;
    double playout_ms = -1;
    double ratio = 1.0;
    float peak = 0;
    float peak_max = 0;
    float rms = 0;
};

// Live console sound: /audio -> ring buffer -> SDL output device.
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();
    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    void set_enabled(bool on);
    bool enabled() const;
    void set_volume(float volume);
    float volume() const;
    void set_delay_ms(int delay_ms);
    int delay_ms() const;
    void set_delay_auto(bool on);
    bool delay_auto() const;
    int effective_delay_ms() const;

    bool active() const;
    AudioStats stats() const;
    std::string last_error() const;

    // Backend only (the App drives these).
    using Clock = std::chrono::steady_clock;
    // Converted stereo float of every packet, before volume, with its arrival time.
    using Sink = std::function<void(const float* stereo, size_t frames, int rate, Clock::time_point arrival)>;

    bool start_session(const std::string& url, const StreamConfig& config);
    void stop_session();
    void set_auto_floor_ms(int ms);
    // Arrival-to-speaker latency in ms, or -1 when not playing.
    double playout_ms() const;
    void set_sink(Sink sink);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

// Threads in this process (Toolhelp snapshot), -1 if unavailable.
int process_thread_count();

// Shared SDL audio subsystem reference count (live sound and the media player).
bool sdl_audio_acquire(std::string& error);
void sdl_audio_release();

}
