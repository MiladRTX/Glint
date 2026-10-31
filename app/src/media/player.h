#pragma once
#include "decoder.h"
#include <memory>
#include <string>

namespace glint {

// Plays one saved recording in the gallery viewer. Methods are called from
// the UI thread; decoding runs on a worker. The constructor does no work so a
// static instance is safe.
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();
    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    bool open(const std::string& path, std::string& error);
    void close();

    void play();
    void pause();
    bool playing() const;
    bool ended() const;
    void seek(double seconds);
    void set_volume(float volume);
    float volume() const;

    double duration() const;
    double position() const;
    bool has_audio() const;
    int width() const;
    int height() const;

    // The newest picture that is due, as RGBA; false when nothing new.
    bool take_frame(Picture& picture);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    float volume_ = 1.0f;
};

}
