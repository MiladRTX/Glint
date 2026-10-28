#pragma once

#include "audio.h"
#include "frame_grid.h"
#include "glint/payload_client.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;

namespace glint {

struct RecorderInfo {
    enum class State { Idle, Armed, Recording, Finalizing, Failed };
    State state = State::Idle;
    std::string path;       // current or last file
    std::string message;    // last status line
    std::string error;
    uint64_t video_frames = 0;
    uint64_t audio_frames = 0;   // PCM frames encoded
    uint64_t dropped = 0;        // packets dropped on backpressure
    uint64_t bytes = 0;
    double seconds = 0.0;
    bool has_audio = false;
    int width = 0;
    int height = 0;
};

const char* recorder_state_name(RecorderInfo::State state);

// Local recording: payload H.264 is stream-copied (no re-encode), PCM is
// encoded to AAC, both go to a Matroska file (crash tolerant). Packets are
// queued by the network threads and written by a dedicated thread, so disk
// stalls never block decoding. The file starts at the first keyframe.
class LocalRecorder : public AudioSink {
public:
    LocalRecorder() = default;
    ~LocalRecorder() override;

    LocalRecorder(const LocalRecorder&) = delete;
    LocalRecorder& operator=(const LocalRecorder&) = delete;

    // `dir` must be absolute. Opens the file and writes the header now (so
    // errors surface here), then waits for a keyframe to start writing.
    bool start(const std::string& dir, const StreamConfig& config, bool with_audio,
               std::string& error);
    // Finalizes asynchronously (state Finalizing -> Idle). `reason` is shown
    // in the status line when non-empty.
    void stop(const std::string& reason = std::string());
    void wait();   // blocks until the writer thread has finished

    bool active() const;   // armed, recording or finalizing
    bool recording() const { return capturing_.load(); }
    RecorderInfo info() const;

    void push_video(const uint8_t* annexb, size_t len, uint32_t pts90k);
    void on_audio(const float* stereo, size_t frames, int rate) override;

private:
    struct Item {
        bool video = false;
        bool key = false;
        uint32_t raw_pts = 0;
        std::chrono::steady_clock::time_point arrival;
        std::vector<uint8_t> bytes;    // AVCC access unit
        std::vector<float> samples;    // interleaved stereo
    };

    void run();
    void write_video(Item& item);
    void write_audio(Item& item);
    void encode_audio(bool flush);
    void write_packet_error(int code, const char* what);
    void finalize();
    void set_message(const std::string& text);
    void set_failed(const std::string& text);

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    size_t queued_bytes_ = 0;
    bool stopping_ = false;
    std::atomic_bool capturing_{false};   // accepts packets (armed or recording)
    std::atomic_bool started_flag_{false};

    RecorderInfo info_;
    std::string stop_reason_;

    // Writer-thread state ---------------------------------------------------
    AVFormatContext* fmt_ = nullptr;
    AVStream* video_stream_ = nullptr;
    AVStream* audio_stream_ = nullptr;
    AVCodecContext* aac_ = nullptr;
    bool header_written_ = false;
    bool failed_ = false;
    bool started_ = false;
    int fps_ = 60;
    int audio_rate_ = 48000;
    uint32_t last_raw_ = 0;
    int64_t unwrapped_ = 0;
    int64_t first_pts_ = 0;
    int64_t last_dts_ = -1;
    FrameGrid grid_;
    std::chrono::steady_clock::time_point video_start_wall_{};
    bool audio_aligned_ = false;
    int64_t audio_next_pts_ = 0;      // in samples
    std::vector<float> audio_fifo_;   // interleaved stereo awaiting a full frame
    uint64_t bytes_written_ = 0;
};

} // namespace glint
