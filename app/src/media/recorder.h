#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace glint {

struct StreamConfig;

struct RecorderInfo {
    enum class State { Idle, Armed, Recording, Finalizing, Failed };
    State state = State::Idle;
    std::string path;
    std::string message;
    std::string error;
    uint64_t video_frames = 0;
    uint64_t audio_frames = 0;
    uint64_t dropped = 0;
    uint64_t bytes = 0;
    double seconds = 0.0;
    bool has_audio = false;
    int width = 0;
    int height = 0;
};

const char* recorder_state_name(RecorderInfo::State state);

// Local MKV recording: the console's H.264 copied as is, sound as AAC.
// Internal to the backend; the UI sees it through App.
class LocalRecorder {
public:
    using Clock = std::chrono::steady_clock;

    LocalRecorder();
    ~LocalRecorder();
    LocalRecorder(const LocalRecorder&) = delete;
    LocalRecorder& operator=(const LocalRecorder&) = delete;

    // Creates the file and writes its header; then waits for a keyframe.
    bool start(const std::string& dir, const StreamConfig& config, bool with_audio, int audio_rate,
               std::string& error);
    // Asks the recording to finish (returns at once). `reason` goes into the message.
    void stop(const std::string& reason);
    // Waits for a finishing recording to close its file.
    void wait();

    void push_video(const uint8_t* annexb, size_t len, uint32_t pts, Clock::time_point arrival);
    void push_audio(const float* stereo, size_t frames, int rate, Clock::time_point arrival);

    RecorderInfo info() const;
    RecorderInfo::State state() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}
