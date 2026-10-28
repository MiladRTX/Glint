#pragma once

#include "audio.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFrame;

namespace glint {

struct BroadcastTarget {
    std::string name;   // shown in the UI
    std::string url;    // full ingest URL including the stream key (never logged)
};

struct BroadcastQuality {
    int height = 1080;          // 480 / 720 / 1080; width follows the source aspect
    int fps = 60;               // 30 or 60
    int video_kbps = 6000;
    int audio_kbps = 160;
};

struct BroadcastDestinationStatus {
    enum class State { Connecting, Live, Failed, Stopped };
    std::string name;
    State state = State::Connecting;
    std::string error;
    uint64_t bytes = 0;
    double kbps = 0.0;
    uint64_t dropped_packets = 0;
    double live_seconds = 0.0;
};

struct BroadcastStatus {
    bool active = false;        // accepting frames
    bool stopping = false;      // workers still finishing
    std::string encoder;        // e.g. "h264_amf"
    std::string error;
    std::string message;
    double seconds = 0.0;
    uint64_t frames_in = 0;
    uint64_t frames_encoded = 0;
    uint64_t frames_dropped = 0;  // encoder could not keep up
    int width = 0;
    int height = 0;
    int fps = 0;
    std::vector<BroadcastDestinationStatus> destinations;
};

const char* broadcast_state_name(BroadcastDestinationStatus::State state);

// Live broadcast to 1-5 RTMP/RTMPS destinations at once.
//
// Decoded console frames are scaled and encoded once (AMD AMF, Media
// Foundation or x264, first that opens), console PCM is encoded to AAC, and
// the packets fan out to one writer thread per destination. A slow or failed
// destination only drops its own packets (back to the next keyframe); it never
// stalls the encoder or the other destinations. Keyframe every 2 s, no
// B-frames, constant bitrate, FLV.
class Broadcaster : public AudioSink {
public:
    Broadcaster();
    ~Broadcaster() override;

    Broadcaster(const Broadcaster&) = delete;
    Broadcaster& operator=(const Broadcaster&) = delete;

    bool start(const std::vector<BroadcastTarget>& targets, const BroadcastQuality& quality,
               bool with_audio, int audio_rate, std::string& error);
    // Signals every worker and returns at once; status().stopping stays true
    // until they have closed their connections.
    void stop(const std::string& reason = std::string());
    void wait();   // joins all workers (used on shutdown and by start())

    bool active() const { return accepting_.load(); }
    BroadcastStatus status() const;

    // Called from the decode thread with the native decoded frame.
    void push_frame(const AVFrame* frame, uint32_t pts90k, std::chrono::steady_clock::time_point arrival);
    void on_audio(const float* stereo, size_t frames, int rate) override;

    struct Impl;   // public so the worker thunks can name it

private:
    std::unique_ptr<Impl> impl_;
    std::atomic_bool accepting_{false};
};

} // namespace glint
