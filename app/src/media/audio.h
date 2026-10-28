#pragma once

#include "glint/payload_client.h"

#include <cstdint>
#include <memory>
#include <string>

namespace glint {

struct AudioStats {
    bool device_open = false;
    bool streaming = false;
    bool primed = false;
    std::string state;        // human readable one-liner
    std::string device;
    std::string format;       // e.g. "pcm_f32le 2ch 48000 Hz"
    int rate = 0;

    uint64_t packets = 0;
    uint64_t frames_in = 0;
    uint64_t frames_out = 0;
    uint64_t callbacks = 0;
    uint64_t drop_events = 0;       // ring overflows
    uint64_t dropped_frames = 0;
    uint64_t underruns = 0;         // callbacks that ran dry after priming
    uint64_t underrun_frames = 0;
    uint64_t underruns_after_warmup = 0;
    bool warmed_up = false;         // first 5 s of playback have elapsed
    uint64_t nonfinite_in = 0;      // sanitised on ingest
    uint64_t nonfinite_out = 0;     // must stay 0: sanitised before SDL
    uint64_t clipped_samples = 0;
    uint64_t reconnects = 0;

    // Timing diagnostics (ms)
    double max_packet_gap_ms = 0;     // longest silence between payload packets
    uint64_t packet_gaps_50ms = 0;    // gaps > 50 ms
    uint64_t packet_gaps_100ms = 0;   // gaps > 100 ms
    uint64_t packet_gaps_200ms = 0;   // gaps > 200 ms
    uint64_t packet_gaps_400ms = 0;   // gaps > 400 ms
    double max_callback_gap_ms = 0;   // longest SDL callback interval
    uint64_t callback_gaps_40ms = 0;  // callback intervals > 40 ms

    double buffered_ms = 0;
    double target_ms = 0;
    bool auto_delay = false;
    double playout_ms = -1;          // packet arrival -> speaker, -1 until primed
    double ratio = 1.0;             // drift correction (1.0 = none)
    float peak = 0;                 // recent output peak
    float peak_max = 0;             // lifetime output peak
    float rms = 0;
};

// Plays the payload /audio stream through SDL2.
//
// Data path: PayloadClient::stream_audio (own thread) -> ring buffer (~3 s,
// drop-oldest) -> SDL callback (1024 samples, F32, 2 ch). The callback never
// allocates; it only holds a spin lock long enough to copy ~1k frames.
//
// Volume / delay / enabled are settings that outlive start()/stop().
// Receives every decoded stereo float frame the payload delivers, whether or
// not playback is enabled (used by the local recorder). Called on the audio
// network thread; must be quick and non-blocking.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual void on_audio(const float* stereo, size_t frames, int rate) = 0;
};

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // Opens the SDL device and starts the network thread. Safe to call from
    // any thread; restarts if already running.
    bool start(const std::string& base_url, const StreamConfig& config);
    // Stops the network thread, closes the device and drains the ring.
    void stop();
    bool active() const;

    void set_enabled(bool on);
    bool enabled() const;
    void set_volume(float volume);      // 0..4
    float volume() const;
    void set_delay_ms(int delay_ms);    // 0..400, prebuffer target (manual mode)
    int delay_ms() const;
    // Auto: size the prebuffer from the stalls actually seen on the link
    // (default). Manual: use delay_ms().
    void set_delay_auto(bool on);
    bool delay_auto() const;
    int effective_delay_ms() const;     // the target in use right now
    // Smallest automatic buffer (Latency: Lowest 100 ms, Smooth 150 ms).
    void set_auto_floor_ms(int ms);
    // Time from a packet arriving to it reaching the speaker. -1 while not
    // playing (stopped, buffering, sound off). Used to hold video in sync.
    double playout_latency_ms() const;

    AudioStats stats() const;
    std::string last_error() const;

    // Register/unregister a sink (recorder, broadcaster). After remove_sink()
    // returns, that sink is guaranteed not to be called again.
    void add_sink(AudioSink* sink);
    void remove_sink(AudioSink* sink);

    struct Impl;   // public so the SDL C callback can name it

private:
    std::unique_ptr<Impl> impl_;
};

// Number of threads in this process (diagnostics: proves audio threads exit).
int process_thread_count();

} // namespace glint
