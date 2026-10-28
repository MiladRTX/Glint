#pragma once

#include "decoder.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

struct AVFormatContext;
struct AVPacket;

namespace glint {

// Plays a recording (MKV / MP4: H.264 + AAC) for the built-in viewer. A worker
// thread demuxes and decodes; video frames wait in a short queue, sound goes
// straight to its own SDL audio device, and the sound card's position is the
// clock the picture follows (the wall clock when there is no sound).
class MediaPlayer {
public:
    MediaPlayer() = default;
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();

    void play();
    void pause();
    bool playing() const { return playing_.load(); }
    bool ended() const { return ended_.load(); }
    // Jumps to `seconds` (clamped), showing the exact frame there.
    void seek(double seconds);
    void set_volume(float volume);   // 0..2
    float volume() const { return volume_.load(); }

    double duration() const { return duration_; }
    double position() const;
    bool has_audio() const { return audio_stream_ >= 0; }
    int width() const { return width_; }
    int height() const { return height_; }

    // The newest frame that is due by now (RGBA), if it differs from the last
    // one taken. Call once per UI frame; older due frames are dropped.
    bool take_frame(Picture& picture);

private:
    struct Frame { double pts; Picture picture; };

    void worker();
    double clock_locked() const;
    bool decode_video(struct AVPacket* packet);
    bool decode_audio(struct AVPacket* packet);
    void queue_audio(const struct AVFrame* frame);
    bool to_rgba(const struct AVFrame* frame, Picture& picture);

    struct SwsContext* sws_ = nullptr;
    struct AVFormatContext* format_ = nullptr;
    struct AVCodecContext* video_ = nullptr;
    struct AVCodecContext* audio_ = nullptr;
    struct AVFrame* frame_ = nullptr;
    int video_stream_ = -1;
    int audio_stream_ = -1;
    double video_tb_ = 0.0, audio_tb_ = 0.0;
    double duration_ = 0.0;
    int width_ = 0, height_ = 0;
    int channels_ = 2, rate_ = 48000;
    uint32_t device_ = 0;   // SDL_AudioDeviceID

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Frame> frames_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> ended_{false};
    std::atomic<float> volume_{1.0f};
    bool demux_done_ = false;
    double seek_to_ = -1.0;          // pending seek (worker)
    double skip_until_ = -1.0;       // drop frames/samples before this after a seek
    double shown_pts_ = -1.0;

    // Clock. With sound: audio_base_ + samples the card has played / rate.
    // Without: wall_base_ + time since wall_start_ while playing.
    double audio_base_ = -1.0;
    uint64_t audio_samples_ = 0;
    double wall_base_ = 0.0;
    double wall_start_ = 0.0;
};

} // namespace glint
