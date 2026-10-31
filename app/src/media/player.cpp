#include "player.h"
#include "audio.h"
#include "convert.h"
#include "log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace glint {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxPictures = 12;
constexpr double kAudioAhead = 0.25;     // seconds of sound queued ahead
constexpr double kDueSlack = 0.004;
constexpr size_t kMaxVideoPackets = 2000;

struct Frame {
    double t = 0.0;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

double secs_since(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}

}  // namespace

struct MediaPlayer::Impl {
    // Demux/decode (worker thread only, except where noted).
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    int vi = -1, ai = -1;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* held = nullptr;   // last picture at or before a seek target
    SwsContext* sws = nullptr;
    std::deque<AVPacket*> vpackets;
    bool demux_eof = false;
    bool video_flushed = false;
    bool audio_flushed = false;
    double last_video_t = 0.0;
    double drop_before = -1.0;   // after a seek: pictures before this are skipped
    std::thread worker;

    // Shared with the UI thread, under m.
    std::mutex m;
    std::condition_variable cv;
    bool quit = false;
    bool seek_pending = false;
    double seek_target = 0.0;
    bool seek_settling = false;   // position() reports the target until a picture is ready
    std::deque<Frame> pictures;
    bool show_first = true;
    bool playing = false;
    bool ended = false;
    bool decode_done = false;     // everything decoded
    double duration = 0.0;
    double last_picture_t = 0.0;  // time of the newest queued picture
    int width = 0, height = 0;

    // Clock: the sound device's played position, else the wall clock.
    bool audio_clock = false;
    double wall_media = 0.0;
    Clock::time_point wall_start;
    bool wall_running = false;

    // Sound, under ring_m (the device callback takes it briefly).
    SDL_AudioDeviceID dev = 0;
    bool sdl_held = false;
    int rate = 0, channels = 0;
    std::mutex ring_m;
    std::vector<float> ring;
    size_t cap = 0, rd = 0, count = 0;   // in sample frames
    double audio_base = 0.0;             // media time of the first frame played since the last seek
    uint64_t played = 0;
    double ring_end_t = 0.0;             // media time just after the queued sound
    bool audio_done = false;
    std::atomic<float> volume{1.0f};
    float gain = 1.0f;
    size_t last_fill_got = 0;            // frames handed to the device by the last callback
    Clock::time_point last_fill_time;
    bool audio_paused = true;            // under m
    double paused_at = 0.0;

    // --- clock ---------------------------------------------------------------------

    // m held.
    double clock_now()
    {
        if (audio_clock) {
            if (audio_paused) return paused_at;
            std::lock_guard<std::mutex> rl(ring_m);
            // Played frames advance once per device callback; interpolate in between
            // so pictures don't arrive in pairs.
            double extra = 0.0;
            if (last_fill_got > 0)
                extra = std::min(secs_since(last_fill_time) * rate, (double)last_fill_got);
            double a = audio_base + ((double)(played - last_fill_got) + extra) / rate;
            if (audio_done && count == 0) {
                // The sound ended before the picture: the wall clock carries on from here.
                audio_clock = false;
                a = audio_base + (double)played / rate;
                wall_media = a;
                wall_start = Clock::now();
                wall_running = playing;
            }
            return a;
        }
        return wall_running ? wall_media + secs_since(wall_start) : wall_media;
    }

    // m held.
    void set_running(bool run)
    {
        if (audio_clock) {
            if (!run && !audio_paused) {
                paused_at = clock_now();
                audio_paused = true;
            } else if (run && audio_paused) {
                audio_paused = false;
                std::lock_guard<std::mutex> rl(ring_m);
                last_fill_got = 0;   // resume from what was really played
            }
            SDL_PauseAudioDevice(dev, run ? 0 : 1);
            return;
        }
        if (run == wall_running) return;
        wall_media = clock_now();
        wall_start = Clock::now();
        wall_running = run;
    }

    // m held. True once all pictures and sound have been shown and played.
    void check_end()
    {
        if (ended || !decode_done || !pictures.empty() || seek_pending || seek_settling) return;
        double now = clock_now();
        if (audio_clock) return;   // sound still playing
        if (now + 1e-3 < last_picture_t) return;
        ended = true;
        playing = false;
        if (dev) SDL_PauseAudioDevice(dev, 1);
        audio_paused = true;
        paused_at = duration;
        wall_running = false;
        wall_media = duration;
    }

    // --- sound -------------------------------------------------------------------------

    static void SDLCALL callback(void* user, Uint8* stream, int len)
    {
        ((Impl*)user)->fill((float*)stream, (size_t)len / sizeof(float));
    }

    void fill(float* out, size_t samples)
    {
        const size_t ch = (size_t)channels;
        size_t frames = samples / ch, got = 0;
        float g1 = volume.load(), g0 = gain;
        {
            std::lock_guard<std::mutex> rl(ring_m);
            got = std::min(frames, count);
            size_t idx = rd;
            for (size_t i = 0; i < got; i++) {
                float g = g0 + (g1 - g0) * (float)(i + 1) / (float)frames;
                for (size_t c = 0; c < ch; c++) {
                    float v = ring[idx * ch + c] * g;
                    out[i * ch + c] = std::isfinite(v) ? std::clamp(v, -1.0f, 1.0f) : 0.0f;
                }
                if (++idx == cap) idx = 0;
            }
            rd = idx;
            count -= got;
            played += got;
            last_fill_got = got;
            last_fill_time = Clock::now();
        }
        std::fill(out + got * ch, out + samples, 0.0f);
        gain = g1;
    }

    // Appends interleaved float sound that starts at media time `t` (worker).
    void queue_audio(const float* data, size_t frames, double t)
    {
        const size_t ch = (size_t)channels;
        std::lock_guard<std::mutex> rl(ring_m);
        // Line the block up with what is queued: trim overlap, fill small gaps with silence.
        double gap = t - ring_end_t;
        if (gap < 0) {
            size_t skip = (size_t)std::llround(-gap * rate);
            if (skip >= frames) return;
            data += skip * ch;
            frames -= skip;
        } else if (gap > 0.5 / rate) {
            size_t pad = std::min((size_t)std::llround(gap * rate), cap - count);
            for (size_t i = 0; i < pad; i++) {
                size_t w = (rd + count) % cap;
                for (size_t c = 0; c < ch; c++) ring[w * ch + c] = 0.0f;
                count++;
            }
            ring_end_t += (double)pad / rate;
        }
        size_t n = std::min(frames, cap - count);
        for (size_t i = 0; i < n; i++) {
            size_t w = (rd + count) % cap;
            for (size_t c = 0; c < ch; c++) ring[w * ch + c] = data[i * ch + c];
            count++;
        }
        ring_end_t += (double)n / rate;
    }

    double queued_audio_seconds()
    {
        std::lock_guard<std::mutex> rl(ring_m);
        return rate ? (double)count / rate : 0.0;
    }

    void decode_audio_frame(AVFrame* f, double tb_time)
    {
        const int ch = channels;
        const int n = f->nb_samples;
        if (n <= 0 || f->ch_layout.nb_channels < ch) return;
        static thread_local std::vector<float> buf;
        buf.resize((size_t)n * ch);
        AVSampleFormat sf = (AVSampleFormat)f->format;
        bool planar = av_sample_fmt_is_planar(sf);
        for (int c = 0; c < ch; c++) {
            for (int i = 0; i < n; i++) {
                const uint8_t* p = planar ? f->extended_data[c] : f->extended_data[0];
                size_t idx = planar ? (size_t)i : (size_t)i * f->ch_layout.nb_channels + c;
                float v = 0.0f;
                switch (av_get_packed_sample_fmt(sf)) {
                case AV_SAMPLE_FMT_FLT: v = ((const float*)p)[idx]; break;
                case AV_SAMPLE_FMT_DBL: v = (float)((const double*)p)[idx]; break;
                case AV_SAMPLE_FMT_S16: v = ((const int16_t*)p)[idx] / 32768.0f; break;
                case AV_SAMPLE_FMT_S32: v = (float)(((const int32_t*)p)[idx] / 2147483648.0); break;
                case AV_SAMPLE_FMT_U8: v = (((const uint8_t*)p)[idx] - 128) / 128.0f; break;
                default: break;
                }
                buf[(size_t)i * ch + c] = v;
            }
        }
        queue_audio(buf.data(), (size_t)n, tb_time);
    }

    // --- worker ------------------------------------------------------------------------

    double stream_time(int64_t ts, int index)
    {
        if (ts == AV_NOPTS_VALUE) return -1.0;
        double t = ts * av_q2d(fmt->streams[index]->time_base);
        if (fmt->start_time != AV_NOPTS_VALUE) t -= fmt->start_time / (double)AV_TIME_BASE;
        return t;
    }

    void push_picture(AVFrame* f, double t)
    {
        Frame out;
        out.t = t;
        if (!frame_to_rgba(f, out.rgba, &sws)) return;
        out.width = f->width;
        out.height = f->height;
        std::lock_guard<std::mutex> lock(m);
        last_picture_t = out.t;
        pictures.push_back(std::move(out));
        seek_settling = false;
    }

    void flush_held()
    {
        if (held && held->width > 0) {
            push_picture(held, std::max(0.0, last_video_t));
            av_frame_unref(held);
        }
    }

    void on_video_frame(AVFrame* f)
    {
        double t = stream_time(f->best_effort_timestamp, vi);
        if (t < 0) t = last_video_t + 1.0 / 60.0;
        if (drop_before >= 0) {
            if (t <= drop_before + 1e-3) {
                // Before the target: keep only the latest one (it is the picture at the target).
                av_frame_unref(held);
                av_frame_ref(held, f);
                last_video_t = t;
                return;
            }
            drop_before = -1.0;
            double held_t = last_video_t;
            if (held->width > 0) {
                push_picture(held, held_t);
                av_frame_unref(held);
            }
        }
        last_video_t = t;
        push_picture(f, t);
    }

    void drain_video()
    {
        while (avcodec_receive_frame(vdec, frame) == 0) {
            on_video_frame(frame);
            av_frame_unref(frame);
        }
    }

    void drain_audio()
    {
        while (adec && avcodec_receive_frame(adec, frame) == 0) {
            double t = stream_time(frame->best_effort_timestamp, ai);
            if (t < 0) {
                std::lock_guard<std::mutex> rl(ring_m);
                t = ring_end_t;
            }
            decode_audio_frame(frame, t);
            av_frame_unref(frame);
        }
    }

    void clear_packets()
    {
        for (AVPacket* p : vpackets) av_packet_free(&p);
        vpackets.clear();
    }

    void do_seek(double target)
    {
        int64_t ts = (int64_t)(target * AV_TIME_BASE);
        if (fmt->start_time != AV_NOPTS_VALUE) ts += fmt->start_time;
        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(vdec);
        if (adec) avcodec_flush_buffers(adec);
        clear_packets();
        av_frame_unref(held);
        demux_eof = video_flushed = audio_flushed = false;
        drop_before = target;
        last_video_t = target;
        {
            std::lock_guard<std::mutex> rl(ring_m);
            rd = count = 0;
            played = 0;
            last_fill_got = 0;
            audio_base = target;
            ring_end_t = target;
            audio_done = false;
        }
        std::lock_guard<std::mutex> lock(m);
        pictures.clear();
        last_picture_t = target;
        show_first = true;
        decode_done = false;
        ended = false;
        audio_clock = dev != 0;
        wall_media = target;
        wall_start = Clock::now();
        wall_running = playing && !audio_clock;
        audio_paused = !playing;
        paused_at = target;
        if (dev) SDL_PauseAudioDevice(dev, playing ? 0 : 1);
    }

    void run()
    {
        for (;;) {
            bool need_video, need_audio;
            {
                std::unique_lock<std::mutex> lock(m);
                if (quit) return;
                if (seek_pending) {
                    double target = seek_target;
                    seek_pending = false;
                    lock.unlock();
                    do_seek(target);
                    continue;
                }
                need_video = pictures.size() < kMaxPictures;
            }
            need_audio = adec && !audio_flushed && queued_audio_seconds() < kAudioAhead;

            bool worked = false;
            if (!demux_eof && ((need_video && vpackets.empty()) || need_audio) && vpackets.size() < kMaxVideoPackets) {
                int rc = av_read_frame(fmt, pkt);
                if (rc < 0) {
                    demux_eof = true;
                } else if (pkt->stream_index == vi) {
                    vpackets.push_back(av_packet_clone(pkt));
                } else if (pkt->stream_index == ai && adec) {
                    if (avcodec_send_packet(adec, pkt) >= 0) drain_audio();
                }
                av_packet_unref(pkt);
                worked = true;
            } else if (need_video && !vpackets.empty()) {
                AVPacket* p = vpackets.front();
                vpackets.pop_front();
                avcodec_send_packet(vdec, p);
                av_packet_free(&p);
                drain_video();
                worked = true;
            } else if (need_video && demux_eof && !video_flushed) {
                avcodec_send_packet(vdec, nullptr);
                drain_video();
                if (drop_before >= 0) {
                    flush_held();
                    drop_before = -1.0;
                }
                video_flushed = true;
                worked = true;
            }
            if (demux_eof && adec && !audio_flushed && need_audio) {
                avcodec_send_packet(adec, nullptr);
                drain_audio();
                audio_flushed = true;
                worked = true;
            }
            if (demux_eof && (!adec || audio_flushed)) {
                std::lock_guard<std::mutex> rl(ring_m);
                audio_done = true;
            }
            if (demux_eof && video_flushed && (!adec || audio_flushed)) {
                std::lock_guard<std::mutex> lock(m);
                decode_done = true;
                if (seek_settling && pictures.empty()) seek_settling = false;
            }
            if (!worked) {
                std::unique_lock<std::mutex> lock(m);
                cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return quit || seek_pending; });
            }
        }
    }

    void release()
    {
        clear_packets();
        if (dev) {
            SDL_CloseAudioDevice(dev);
            dev = 0;
        }
        if (sdl_held) {
            sdl_audio_release();
            sdl_held = false;
        }
        if (vdec) avcodec_free_context(&vdec);
        if (adec) avcodec_free_context(&adec);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (held) av_frame_free(&held);
        if (sws) sws_freeContext(sws);
        sws = nullptr;
        if (fmt) avformat_close_input(&fmt);
    }

    bool open_audio(std::string& why)
    {
        if (ai < 0) return false;
        AVCodecParameters* par = fmt->streams[ai]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec) {
            why = "no decoder for the sound";
            return false;
        }
        adec = avcodec_alloc_context3(codec);
        if (!adec || avcodec_parameters_to_context(adec, par) < 0 || avcodec_open2(adec, codec, nullptr) < 0) {
            if (adec) avcodec_free_context(&adec);
            why = "the sound decoder could not be opened";
            return false;
        }
        rate = adec->sample_rate;
        channels = std::clamp(adec->ch_layout.nb_channels, 1, 8);
        if (rate <= 0) {
            avcodec_free_context(&adec);
            why = "the sound has no sample rate";
            return false;
        }
        std::string error;
        if (!sdl_audio_acquire(error)) {
            avcodec_free_context(&adec);
            why = error;
            return false;
        }
        sdl_held = true;
        SDL_AudioSpec want{}, have{};
        want.freq = rate;
        want.format = AUDIO_F32SYS;
        want.channels = (Uint8)channels;
        want.samples = 1024;
        want.callback = &Impl::callback;
        want.userdata = this;
        dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!dev) {
            why = SDL_GetError();
            avcodec_free_context(&adec);
            sdl_audio_release();
            sdl_held = false;
            return false;
        }
        cap = (size_t)rate;   // one second of room; about a quarter is used
        ring.assign(cap * (size_t)channels, 0.0f);
        rd = count = 0;
        return true;
    }
};

MediaPlayer::MediaPlayer() = default;

MediaPlayer::~MediaPlayer()
{
    close();
}

bool MediaPlayer::open(const std::string& path, std::string& error)
{
    close();
    ffmpeg_setup();
    std::unique_ptr<Impl> p(new Impl);
    Impl& d = *p;
    d.volume.store(volume_);
    if (avformat_open_input(&d.fmt, path.c_str(), nullptr, nullptr) < 0) {
        error = "This recording could not be opened.";
        return false;
    }
    avformat_find_stream_info(d.fmt, nullptr);
    d.vi = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d.vi < 0) {
        d.release();
        error = "This file has no video.";
        return false;
    }
    AVCodecParameters* vpar = d.fmt->streams[d.vi]->codecpar;
    const AVCodec* vcodec = avcodec_find_decoder(vpar->codec_id);
    d.vdec = vcodec ? avcodec_alloc_context3(vcodec) : nullptr;
    if (!d.vdec || avcodec_parameters_to_context(d.vdec, vpar) < 0) {
        d.release();
        error = "The video in this file can't be decoded.";
        return false;
    }
    d.vdec->thread_count = 0;
    if (avcodec_open2(d.vdec, vcodec, nullptr) < 0) {
        d.release();
        error = "The video decoder could not be opened.";
        return false;
    }
    d.pkt = av_packet_alloc();
    d.frame = av_frame_alloc();
    d.held = av_frame_alloc();
    d.width = vpar->width;
    d.height = vpar->height;
    if (d.fmt->duration != AV_NOPTS_VALUE && d.fmt->duration > 0) d.duration = d.fmt->duration / (double)AV_TIME_BASE;
    else {
        AVStream* vs = d.fmt->streams[d.vi];
        if (vs->duration != AV_NOPTS_VALUE && vs->duration > 0) d.duration = vs->duration * av_q2d(vs->time_base);
    }

    d.ai = av_find_best_stream(d.fmt, AVMEDIA_TYPE_AUDIO, -1, d.vi, nullptr, 0);
    std::string why;
    if (d.ai >= 0 && !d.open_audio(why)) {
        logf("viewer: playing without sound (%s)", why.c_str());
        d.ai = -1;
    }
    if (d.ai < 0) d.ai = -1;
    d.audio_clock = d.dev != 0;
    d.wall_start = Clock::now();
    d.seek_settling = true;
    impl_ = std::move(p);
    Impl* raw = impl_.get();
    impl_->worker = std::thread([raw] { raw->run(); });
    return true;
}

void MediaPlayer::close()
{
    if (!impl_) return;
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->quit = true;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->release();
    impl_.reset();
}

void MediaPlayer::play()
{
    if (!impl_) return;
    bool restart = false;
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        restart = impl_->ended;
    }
    if (restart) seek(0.0);
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->playing = true;
    impl_->set_running(true);
}

void MediaPlayer::pause()
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->playing = false;
    impl_->set_running(false);
}

bool MediaPlayer::playing() const
{
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->check_end();
    return impl_->playing;
}

bool MediaPlayer::ended() const
{
    if (!impl_) return false;
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->check_end();
    return impl_->ended;
}

void MediaPlayer::seek(double seconds)
{
    if (!impl_) return;
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    double hi = std::max(0.0, d.duration - 0.05);
    if (!std::isfinite(seconds)) seconds = 0.0;
    d.seek_target = std::clamp(seconds, 0.0, hi);
    d.seek_pending = true;
    d.seek_settling = true;
    d.ended = false;
    d.cv.notify_all();
}

void MediaPlayer::set_volume(float v)
{
    if (!std::isfinite(v)) v = 1.0f;
    volume_ = std::clamp(v, 0.0f, 2.0f);
    if (impl_) impl_->volume.store(volume_);
}

float MediaPlayer::volume() const
{
    return volume_;
}

double MediaPlayer::duration() const
{
    return impl_ ? impl_->duration : 0.0;
}

double MediaPlayer::position() const
{
    if (!impl_) return 0.0;
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.m);
    d.check_end();
    if (d.ended) return d.duration;
    if (d.seek_pending || d.seek_settling) return d.seek_target;
    double t = d.clock_now();
    return d.duration > 0 ? std::clamp(t, 0.0, d.duration) : std::max(t, 0.0);
}

bool MediaPlayer::has_audio() const
{
    return impl_ && impl_->dev != 0;
}

int MediaPlayer::width() const
{
    return impl_ ? impl_->width : 0;
}

int MediaPlayer::height() const
{
    return impl_ ? impl_->height : 0;
}

bool MediaPlayer::take_frame(Picture& picture)
{
    if (!impl_) return false;
    Impl& d = *impl_;
    Frame out;
    {
        std::lock_guard<std::mutex> lock(d.m);
        if (d.pictures.empty()) {
            d.check_end();
            return false;
        }
        if (d.show_first) {
            out = std::move(d.pictures.front());
            d.pictures.pop_front();
            d.show_first = false;
        } else {
            if (d.seek_pending) return false;
            double now = d.clock_now();
            size_t due = 0;
            while (due < d.pictures.size() && d.pictures[due].t <= now + kDueSlack) due++;
            if (due == 0) return false;
            out = std::move(d.pictures[due - 1]);
            d.pictures.erase(d.pictures.begin(), d.pictures.begin() + (ptrdiff_t)due);
        }
    }
    d.cv.notify_all();
    picture.format = Picture::Format::Rgba;
    picture.width = out.width;
    picture.height = out.height;
    picture.bt709 = true;
    picture.full_range = true;
    picture.data = std::move(out.rgba);
    return true;
}

}
