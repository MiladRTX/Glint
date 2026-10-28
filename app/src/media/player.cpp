#include "player.h"

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace glint {

namespace {

double now_seconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

AVCodecContext* open_decoder(AVStream* stream, int threads, std::string& error) {
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        error = "This file uses a format Glint cannot play.";
        return nullptr;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
        avcodec_free_context(&ctx);
        error = "Could not prepare the decoder.";
        return nullptr;
    }
    ctx->thread_count = threads;   // a file can use frame threads: latency does not matter here
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        error = "Could not open the decoder.";
        return nullptr;
    }
    return ctx;
}

} // namespace

// RGBA on the worker thread, so the UI thread only uploads. (The live view's
// GPU converter has one set of buffers; a second picture per frame through it
// would race with the live one.)
bool MediaPlayer::to_rgba(const AVFrame* f, Picture& pic) {
    sws_ = sws_getCachedContext(sws_, f->width, f->height, (AVPixelFormat)f->format, f->width, f->height,
                                AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) return false;
    // BT.709 for HD unless the stream says otherwise; swscale defaults to BT.601.
    bool bt709 = f->colorspace == AVCOL_SPC_BT709 ||
                 (f->colorspace != AVCOL_SPC_BT470BG && f->colorspace != AVCOL_SPC_SMPTE170M && f->height >= 720);
    bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    sws_setColorspaceDetails(sws_, sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601), full ? 1 : 0,
                             sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    pic.format = Picture::Format::Rgba;
    pic.width = f->width;
    pic.height = f->height;
    pic.data.resize((size_t)f->width * f->height * 4);
    uint8_t* dst[4] = {pic.data.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {f->width * 4, 0, 0, 0};
    sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dst_stride);
    return true;
}

MediaPlayer::~MediaPlayer() {
    close();
}

bool MediaPlayer::open(const std::string& path, std::string& error) {
    close();
    av_log_set_level(AV_LOG_QUIET);
    if (avformat_open_input(&format_, path.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(format_, nullptr) < 0) {
        error = "Could not open the recording.";
        close();
        return false;
    }
    video_stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    audio_stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, video_stream_, nullptr, 0);
    if (video_stream_ < 0) {
        error = "The file has no picture.";
        close();
        return false;
    }
    AVStream* vs = format_->streams[video_stream_];
    if (!(video_ = open_decoder(vs, 0, error))) {
        close();
        return false;
    }
    video_tb_ = av_q2d(vs->time_base);
    width_ = video_->width;
    height_ = video_->height;
    if (format_->duration > 0) duration_ = (double)format_->duration / AV_TIME_BASE;
    else if (vs->duration > 0) duration_ = vs->duration * video_tb_;

    if (audio_stream_ >= 0) {
        std::string audio_error;
        AVStream* as = format_->streams[audio_stream_];
        audio_ = open_decoder(as, 1, audio_error);
        if (audio_ && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            audio_tb_ = av_q2d(as->time_base);
            rate_ = audio_->sample_rate;
            channels_ = std::clamp(audio_->ch_layout.nb_channels, 1, 8);
            SDL_AudioSpec want = {}, have = {};
            want.freq = rate_;
            want.format = AUDIO_F32SYS;
            want.channels = (Uint8)channels_;
            want.samples = 1024;
            device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
            if (!device_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
        if (!device_) {   // play silently rather than not at all
            if (audio_) avcodec_free_context(&audio_);
            audio_stream_ = -1;
        }
    }
    frame_ = av_frame_alloc();
    stop_.store(false);
    ended_.store(false);
    playing_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        demux_done_ = false;
        seek_to_ = -1.0;
        skip_until_ = 0.0;
        shown_pts_ = -1.0;
        audio_base_ = -1.0;
        audio_samples_ = 0;
        wall_base_ = 0.0;
        wall_start_ = now_seconds();
    }
    thread_ = std::thread(&MediaPlayer::worker, this);
    return true;
}

void MediaPlayer::close() {
    stop_.store(true);
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (device_) {
        SDL_CloseAudioDevice(device_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        device_ = 0;
    }
    if (frame_) av_frame_free(&frame_);
    if (sws_) {
        sws_freeContext(sws_);
        sws_ = nullptr;
    }
    if (video_) avcodec_free_context(&video_);
    if (audio_) avcodec_free_context(&audio_);
    if (format_) avformat_close_input(&format_);
    video_stream_ = audio_stream_ = -1;
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    playing_.store(false);
}

void MediaPlayer::play() {
    if (ended_.load()) seek(0.0);
    std::lock_guard<std::mutex> lock(mutex_);
    if (playing_.load()) return;
    wall_start_ = now_seconds();
    playing_.store(true);
    if (device_) SDL_PauseAudioDevice(device_, 0);
    wake_.notify_all();
}

void MediaPlayer::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_.load()) return;
    wall_base_ = clock_locked();
    playing_.store(false);
    if (device_) SDL_PauseAudioDevice(device_, 1);
}

void MediaPlayer::seek(double seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    seek_to_ = std::clamp(seconds, 0.0, std::max(0.0, duration_ - 0.05));
    frames_.clear();
    ended_.store(false);
    shown_pts_ = -1.0;
    audio_base_ = -1.0;
    audio_samples_ = 0;
    wall_base_ = seek_to_;
    wall_start_ = now_seconds();
    wake_.notify_all();
}

void MediaPlayer::set_volume(float volume) {
    volume_.store(std::clamp(volume, 0.0f, 2.0f));
}

double MediaPlayer::position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::clamp(clock_locked(), 0.0, std::max(0.0, duration_));
}

double MediaPlayer::clock_locked() const {
    if (seek_to_ >= 0.0) return seek_to_;
    if (device_ && audio_base_ >= 0.0) {
        uint64_t queued = SDL_GetQueuedAudioSize(device_) / (sizeof(float) * channels_);
        uint64_t played = audio_samples_ > queued ? audio_samples_ - queued : 0;
        return audio_base_ + (double)played / rate_;
    }
    return wall_base_ + (playing_.load() ? now_seconds() - wall_start_ : 0.0);
}

bool MediaPlayer::take_frame(Picture& picture) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty()) return false;
    double clock = clock_locked();
    // Paused, or waiting for sound to start: show the first frame we have.
    bool show_first = shown_pts_ < 0.0 && (!playing_.load() || (device_ && audio_base_ < 0.0));
    if (!show_first && frames_.front().pts > clock + 0.004) return false;
    while (!show_first && frames_.size() > 1 && frames_[1].pts <= clock + 0.004) frames_.pop_front();
    picture = std::move(frames_.front().picture);
    shown_pts_ = frames_.front().pts;
    frames_.pop_front();
    wake_.notify_all();
    return true;
}

void MediaPlayer::queue_audio(const AVFrame* f) {
    int n = f->nb_samples, ch = channels_;
    double pts = (f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts) * audio_tb_;
    int skip = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (skip_until_ > pts) skip = std::min(n, (int)((skip_until_ - pts) * rate_));
        if (skip >= n) return;
        if (audio_base_ < 0.0) {
            audio_base_ = pts + (double)skip / rate_;
            audio_samples_ = 0;
        }
    }
    float vol = volume_.load();
    std::vector<float> out((size_t)(n - skip) * ch);
    bool planar = av_sample_fmt_is_planar((AVSampleFormat)f->format);
    for (int i = skip; i < n; i++)
        for (int c = 0; c < ch; c++) {
            float s = 0.0f;
            int src_c = std::min(c, f->ch_layout.nb_channels - 1);
            if (f->format == AV_SAMPLE_FMT_FLTP || f->format == AV_SAMPLE_FMT_FLT)
                s = planar ? ((const float*)f->extended_data[src_c])[i] : ((const float*)f->data[0])[i * f->ch_layout.nb_channels + src_c];
            else if (f->format == AV_SAMPLE_FMT_S16P || f->format == AV_SAMPLE_FMT_S16)
                s = (planar ? ((const int16_t*)f->extended_data[src_c])[i]
                            : ((const int16_t*)f->data[0])[i * f->ch_layout.nb_channels + src_c]) / 32768.0f;
            out[(size_t)(i - skip) * ch + c] = std::clamp(s * vol, -1.0f, 1.0f);
        }
    SDL_QueueAudio(device_, out.data(), (Uint32)(out.size() * sizeof(float)));
    std::lock_guard<std::mutex> lock(mutex_);
    audio_samples_ += (uint64_t)(n - skip);
}

bool MediaPlayer::decode_video(AVPacket* packet) {
    if (avcodec_send_packet(video_, packet) < 0 && packet) return false;
    while (avcodec_receive_frame(video_, frame_) >= 0) {
        int64_t ts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
        double pts = ts * video_tb_;
        bool keep;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            keep = pts + 0.001 >= skip_until_;   // after a seek, start exactly there
        }
        if (keep) {
            Frame fr;
            fr.pts = pts;
            if (to_rgba(frame_, fr.picture)) {
                std::lock_guard<std::mutex> lock(mutex_);
                frames_.push_back(std::move(fr));
            }
        }
        av_frame_unref(frame_);
    }
    return true;
}

bool MediaPlayer::decode_audio(AVPacket* packet) {
    if (avcodec_send_packet(audio_, packet) < 0 && packet) return false;
    while (avcodec_receive_frame(audio_, frame_) >= 0) {
        queue_audio(frame_);
        av_frame_unref(frame_);
    }
    return true;
}

void MediaPlayer::worker() {
    AVPacket* packet = av_packet_alloc();
    while (!stop_.load()) {
        double seek = -1.0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (seek_to_ >= 0.0) {
                seek = seek_to_;
            } else {
                double queued_audio = device_ ? (double)SDL_GetQueuedAudioSize(device_) / (sizeof(float) * channels_ * rate_) : 1.0;
                bool video_full = frames_.size() >= 8;
                if ((video_full && queued_audio > 0.25) || frames_.size() >= 24 || (demux_done_ && !frames_.empty())) {
                    wake_.wait_for(lock, std::chrono::milliseconds(5));
                    continue;
                }
                if (demux_done_) {
                    bool sound_left = device_ && SDL_GetQueuedAudioSize(device_) > 0;
                    if (device_ && !sound_left && audio_base_ >= 0.0 && playing_.load()) {
                        // The sound ended first: the wall clock carries the picture on.
                        wall_base_ = clock_locked();
                        wall_start_ = now_seconds();
                        audio_base_ = -1.0;
                    }
                    if (frames_.empty() && !sound_left && playing_.load()) {
                        ended_.store(true);
                        playing_.store(false);
                        wall_base_ = duration_;
                    }
                    wake_.wait_for(lock, std::chrono::milliseconds(10));
                    continue;
                }
            }
        }
        if (seek >= 0.0) {
            av_seek_frame(format_, -1, (int64_t)(seek * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(video_);
            if (audio_) avcodec_flush_buffers(audio_);
            if (device_) SDL_ClearQueuedAudio(device_);
            std::lock_guard<std::mutex> lock(mutex_);
            if (seek_to_ == seek) seek_to_ = -1.0;   // a newer seek waits for the next turn
            skip_until_ = seek;
            demux_done_ = false;
            frames_.clear();
            audio_base_ = -1.0;
            audio_samples_ = 0;
            continue;
        }
        int ret = av_read_frame(format_, packet);
        if (ret < 0) {
            decode_video(nullptr);   // drain the last frames
            if (audio_) decode_audio(nullptr);
            std::lock_guard<std::mutex> lock(mutex_);
            demux_done_ = true;
            continue;
        }
        if (packet->stream_index == video_stream_) decode_video(packet);
        else if (packet->stream_index == audio_stream_ && audio_) decode_audio(packet);
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
}

} // namespace glint
