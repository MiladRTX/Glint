#include "broadcast.h"

#include "frame_grid.h"
#include "log.h"
#include "tls_relay.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace glint {
namespace {

using Clock = std::chrono::steady_clock;

std::string av_error_text(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// With --enable-librtmp, FFmpeg's rtmp/rtmps protocols are librtmp, which
// takes its options space-separated after the URL and ignores rw_timeout.
bool uses_librtmp() {
    static const bool value =
        std::string(avformat_configuration()).find("--enable-librtmp") != std::string::npos;
    return value;
}

struct QueuedPacket {
    AVPacket* packet = nullptr;
    AVRational time_base{1, 1000};
    bool video = false;
};

} // namespace

const char* broadcast_state_name(BroadcastDestinationStatus::State state) {
    switch (state) {
    case BroadcastDestinationStatus::State::Connecting: return "Connecting";
    case BroadcastDestinationStatus::State::Live: return "Live";
    case BroadcastDestinationStatus::State::Failed: return "Failed";
    case BroadcastDestinationStatus::State::Stopped: return "Stopped";
    }
    return "";
}

struct Destination {
    std::string name;
    std::string url;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<QueuedPacket> queue;
    size_t queued_bytes = 0;
    bool waiting_key = true;        // drop until the next video keyframe
    bool accepting = true;          // false once failed/stopped
    bool eof = false;               // encoder finished: drain and close
    Clock::time_point eof_time{};
    std::atomic_bool abort{false};
    // status (guarded by mutex)
    BroadcastDestinationStatus status;
    Clock::time_point live_since{};
    Clock::time_point rate_window{};
    uint64_t rate_bytes = 0;
};

struct Broadcaster::Impl {
    Broadcaster* owner = nullptr;
    BroadcastQuality quality;
    bool with_audio = false;
    int audio_rate = 48000;

    mutable std::mutex status_mutex;
    std::string encoder_name;
    std::string error;
    std::string message;
    Clock::time_point started{};
    std::atomic_bool stopping{false};
    std::atomic<int> running_workers{0};
    std::atomic<uint64_t> frames_in{0}, frames_encoded{0}, frames_dropped{0};
    int out_w = 0, out_h = 0;

    // Input from the decode and audio threads.
    struct Input {
        AVFrame* frame = nullptr;
        uint32_t pts = 0;
        Clock::time_point arrival;
        std::vector<float> samples;
    };
    std::mutex in_mutex;
    std::condition_variable in_cv;
    std::deque<Input> input;
    int queued_video = 0;
    bool input_closed = false;

    std::thread encoder_thread;
    std::vector<std::unique_ptr<Destination>> dests;

    // Encoders become available to the destination threads once the first
    // frame has fixed the picture size.
    std::mutex ready_mutex;
    std::condition_variable ready_cv;
    bool ready = false;
    bool encoder_failed = false;

    // Encoder-thread state.
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    SwsContext* sws = nullptr;
    int src_w = 0, src_h = 0, src_fmt = -1;
    AVPixelFormat enc_fmt = AV_PIX_FMT_NONE;
    bool video_started = false;
    uint32_t last_raw = 0;
    int64_t unwrapped = 0;
    FrameGrid grid;   // console frames on an exact 60 fps timeline
    int64_t last_out_pts = -1;
    int64_t last_key_pts = -1;
    Clock::time_point video_wall0{};
    bool audio_aligned = false;
    int64_t audio_next = 0;
    std::vector<float> fifo;

    void set_error(const std::string& text) {
        std::lock_guard<std::mutex> lock(status_mutex);
        if (error.empty()) error = text;
    }

    // --- encoder thread ---------------------------------------------------

    bool open_video_encoder(int width, int height, AVPixelFormat format) {
        int h = std::min(quality.height, height);
        h &= ~1;
        int w = (int)std::lround((double)width * h / height);
        w &= ~1;
        static const char* candidates[] = {"h264_amf", "h264_mf", "libx264"};
        std::string tried;
        for (const char* name : candidates) {
            const AVCodec* codec = avcodec_find_encoder_by_name(name);
            if (!codec) continue;
            AVCodecContext* ctx = avcodec_alloc_context3(codec);
            if (!ctx) continue;
            const void* configs = nullptr;
            int count = 0;
            AVPixelFormat pick = AV_PIX_FMT_YUV420P;
            if (avcodec_get_supported_config(ctx, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) >= 0 &&
                configs) {
                const AVPixelFormat* list = (const AVPixelFormat*)configs;
                bool has_nv12 = false, has_420 = false;
                for (int i = 0; i < count; i++) {
                    if (list[i] == AV_PIX_FMT_NV12) has_nv12 = true;
                    if (list[i] == AV_PIX_FMT_YUV420P) has_420 = true;
                }
                pick = has_nv12 ? AV_PIX_FMT_NV12 : (has_420 ? AV_PIX_FMT_YUV420P : list[0]);
            }
            ctx->width = w;
            ctx->height = h;
            ctx->pix_fmt = pick;
            ctx->time_base = AVRational{1, quality.fps};
            ctx->framerate = AVRational{quality.fps, 1};
            ctx->gop_size = quality.fps * 2;          // keyframe every 2 s
            ctx->max_b_frames = 0;
            ctx->bit_rate = (int64_t)quality.video_kbps * 1000;
            ctx->rc_max_rate = ctx->bit_rate;
            ctx->rc_min_rate = ctx->bit_rate;
            ctx->rc_buffer_size = (int)ctx->bit_rate;  // 1 s VBV
            ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            ctx->color_range = AVCOL_RANGE_MPEG;
            if (std::strcmp(name, "h264_amf") == 0) {
                av_opt_set(ctx->priv_data, "usage", "lowlatency", 0);
                av_opt_set(ctx->priv_data, "rc", "cbr", 0);
                av_opt_set(ctx->priv_data, "quality", "balanced", 0);
                av_opt_set(ctx->priv_data, "forced_idr", "1", 0);   // forced I-frames become IDR
            } else if (std::strcmp(name, "h264_mf") == 0) {
                av_opt_set(ctx->priv_data, "rate_control", "cbr", 0);
                av_opt_set(ctx->priv_data, "scenario", "live_streaming", 0);
            } else {
                av_opt_set(ctx->priv_data, "preset", "veryfast", 0);
                av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(ctx->priv_data, "x264-params", "nal-hrd=cbr", 0);
                av_opt_set(ctx->priv_data, "forced-idr", "1", 0);
            }
            int ret = avcodec_open2(ctx, codec, nullptr);
            if (ret >= 0 && ctx->extradata_size > 0) {
                venc = ctx;
                enc_fmt = pick;
                out_w = w;
                out_h = h;
                std::lock_guard<std::mutex> lock(status_mutex);
                encoder_name = name;
                logf("broadcast: encoder %s %dx%d@%d %d kbps", name, w, h, quality.fps, quality.video_kbps);
                return true;
            }
            tried += std::string(tried.empty() ? "" : ", ") + name + " (" +
                     (ret < 0 ? av_error_text(ret) : std::string("no stream header")) + ")";
            avcodec_free_context(&ctx);
        }
        set_error("Could not start the live encoder: " + (tried.empty() ? std::string("no H.264 encoder") : tried));
        (void)format;
        return false;
    }

    bool open_audio_encoder() {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec) return false;
        aenc = avcodec_alloc_context3(codec);
        aenc->sample_rate = audio_rate;
        aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&aenc->ch_layout, 2);
        aenc->bit_rate = (int64_t)quality.audio_kbps * 1000;
        aenc->time_base = AVRational{1, audio_rate};
        aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(aenc, codec, nullptr) < 0) {
            avcodec_free_context(&aenc);
            return false;
        }
        return true;
    }

    void fan_out(AVPacket* packet, AVRational tb, bool video) {
        bool key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        size_t cap = std::max<size_t>(2u << 20, (size_t)(quality.video_kbps + quality.audio_kbps) * 125 * 4);
        for (auto& d : dests) {
            std::lock_guard<std::mutex> lock(d->mutex);
            if (!d->accepting) continue;
            if (d->waiting_key) {
                if (!(video && key)) continue;
                d->waiting_key = false;
            }
            if (d->queued_bytes + (size_t)packet->size > cap) {
                // Network can't keep up: skip to the next keyframe.
                d->waiting_key = true;
                d->status.dropped_packets++;
                continue;
            }
            AVPacket* clone = av_packet_clone(packet);
            if (!clone) continue;
            d->queued_bytes += (size_t)clone->size;
            d->queue.push_back(QueuedPacket{clone, tb, video});
            d->cv.notify_one();
        }
    }

    void drain_video() {
        AVPacket* packet = av_packet_alloc();
        while (avcodec_receive_packet(venc, packet) >= 0) {
            packet->stream_index = 0;
            fan_out(packet, venc->time_base, true);
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    void drain_audio() {
        AVPacket* packet = av_packet_alloc();
        while (avcodec_receive_packet(aenc, packet) >= 0) {
            packet->stream_index = 1;
            fan_out(packet, aenc->time_base, false);
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    void handle_video(Input& item) {
        AVFrame* src = item.frame;
        if (!venc) {
            if (encoder_failed) return;
            if (!open_video_encoder(src->width, src->height, (AVPixelFormat)src->format)) {
                encoder_failed = true;
                std::lock_guard<std::mutex> lock(ready_mutex);
                ready_cv.notify_all();
                return;
            }
            std::lock_guard<std::mutex> lock(ready_mutex);
            ready = true;
            ready_cv.notify_all();
        }
        if (!sws || src->width != src_w || src->height != src_h || src->format != src_fmt) {
            if (sws) sws_freeContext(sws);
            sws = sws_getContext(src->width, src->height, (AVPixelFormat)src->format, out_w, out_h, enc_fmt,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            src_w = src->width;
            src_h = src->height;
            src_fmt = src->format;
            if (!sws) {
                set_error("Could not scale broadcast frame.");
                return;
            }
        }

        // Timeline from the console's 90 kHz clock, on an exact 60 fps grid
        // like the recorder (frame_grid.h). Following the jittery clock used to
        // land frames on output slots already taken, and those were dropped.
        if (!video_started) {
            video_started = true;
            last_raw = item.pts;
            unwrapped = 0;
            grid.reset(60);
            video_wall0 = item.arrival;
        } else {
            unwrapped += (int32_t)(item.pts - last_raw);
            last_raw = item.pts;
        }
        int64_t out_pts = grid.place(unwrapped) * quality.fps / 60;
        if (out_pts <= last_out_pts) return;   // 60 -> 30 fps: every other frame
        last_out_pts = out_pts;

        AVFrame* dst = av_frame_alloc();
        dst->format = enc_fmt;
        dst->width = out_w;
        dst->height = out_h;
        if (av_frame_get_buffer(dst, 0) < 0) {
            av_frame_free(&dst);
            set_error("Could not allocate broadcast video frame.");
            return;
        }
        sws_scale(sws, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
        dst->pts = out_pts;
        // Platforms require a keyframe every 2 s. Not every encoder honours
        // gop_size in its low-latency mode, so force it.
        if (last_key_pts < 0 || out_pts - last_key_pts >= (int64_t)quality.fps * 2) {
            dst->pict_type = AV_PICTURE_TYPE_I;
            dst->flags |= AV_FRAME_FLAG_KEY;
            last_key_pts = out_pts;
        }
        int ret = avcodec_send_frame(venc, dst);
        av_frame_free(&dst);
        if (ret < 0) {
            set_error("Could not encode broadcast video frame: " + av_error_text(ret));
            return;
        }
        frames_encoded++;
        drain_video();
    }

    void encode_audio(bool flush) {
        const int fs = aenc->frame_size > 0 ? aenc->frame_size : 1024;
        size_t consumed = 0;
        auto send = [&](const float* inter, int available) {
            AVFrame* frame = av_frame_alloc();
            frame->nb_samples = fs;
            frame->format = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&frame->ch_layout, 2);
            frame->sample_rate = audio_rate;
            if (av_frame_get_buffer(frame, 0) < 0) {
                av_frame_free(&frame);
                return;
            }
            float* l = (float*)frame->extended_data[0];
            float* r = (float*)frame->extended_data[1];
            for (int i = 0; i < fs; i++) {
                l[i] = i < available ? inter[i * 2] : 0.f;
                r[i] = i < available ? inter[i * 2 + 1] : 0.f;
            }
            frame->pts = audio_next;
            audio_next += fs;
            avcodec_send_frame(aenc, frame);
            av_frame_free(&frame);
            drain_audio();
        };
        while (fifo.size() - consumed >= (size_t)fs * 2) {
            send(fifo.data() + consumed, fs);
            consumed += (size_t)fs * 2;
        }
        if (flush) {
            size_t rest = (fifo.size() - consumed) / 2;
            if (rest) send(fifo.data() + consumed, (int)rest);
            consumed = fifo.size();
            avcodec_send_frame(aenc, nullptr);
            drain_audio();
        }
        fifo.erase(fifo.begin(), fifo.begin() + (std::ptrdiff_t)consumed);
    }

    void handle_audio(Input& item) {
        if (!aenc || !video_started || !venc) return;
        size_t frames = item.samples.size() / 2;
        if (!audio_aligned) {
            double offset = std::chrono::duration<double>(item.arrival - video_wall0).count() -
                            (double)frames / audio_rate;
            audio_next = offset > 0 ? (int64_t)std::llround(offset * audio_rate) : 0;
            audio_aligned = true;
        }
        fifo.insert(fifo.end(), item.samples.begin(), item.samples.end());
        encode_audio(false);
    }

    void run_encoder() {
        for (;;) {
            Input item;
            {
                std::unique_lock<std::mutex> lock(in_mutex);
                in_cv.wait(lock, [&] { return !input.empty() || input_closed; });
                if (input.empty()) break;
                item = std::move(input.front());
                input.pop_front();
                if (item.frame) queued_video--;
            }
            if (item.frame) {
                handle_video(item);
                av_frame_free(&item.frame);
            } else {
                handle_audio(item);
            }
        }
        // Flush encoders so the tail reaches the destinations.
        if (venc) {
            avcodec_send_frame(venc, nullptr);
            drain_video();
        }
        if (aenc && video_started) encode_audio(true);
        {
            std::lock_guard<std::mutex> lock(ready_mutex);
            if (!ready) encoder_failed = true;   // never got a frame: release waiting destinations
            ready_cv.notify_all();
        }
        for (auto& d : dests) {
            std::lock_guard<std::mutex> lock(d->mutex);
            d->eof = true;
            d->eof_time = Clock::now();
            d->cv.notify_all();
        }
        running_workers--;
    }

    // --- destination threads ---------------------------------------------

    static int interrupt_cb(void* opaque) {
        Destination* d = static_cast<Destination*>(opaque);
        if (d->abort.load()) return 1;
        std::lock_guard<std::mutex> lock(d->mutex);
        // Give a draining destination 3 s to close cleanly, then cut it.
        return d->eof && Clock::now() - d->eof_time > std::chrono::seconds(3) ? 1 : 0;
    }

    void fail(Destination& d, const std::string& text) {
        logf("broadcast: %s failed: %s", d.name.c_str(), text.c_str());
        std::lock_guard<std::mutex> lock(d.mutex);
        d.status.state = BroadcastDestinationStatus::State::Failed;
        if (d.status.error.empty()) d.status.error = text;
        d.accepting = false;
        for (auto& q : d.queue) av_packet_free(&q.packet);
        d.queue.clear();
        d.queued_bytes = 0;
    }

    void run_destination(Destination& d) {
        {
            std::unique_lock<std::mutex> lock(ready_mutex);
            ready_cv.wait(lock, [&] { return ready || encoder_failed || d.abort.load(); });
            if (!ready) {
                lock.unlock();
                if (encoder_failed && !d.abort.load()) fail(d, "Could not start the live encoder.");
                else {
                    std::lock_guard<std::mutex> g(d.mutex);
                    d.status.state = BroadcastDestinationStatus::State::Stopped;
                    d.accepting = false;
                }
                running_workers--;
                return;
            }
        }

        std::string url = d.url;
        // RTMPS: librtmp's TLS does not verify certificates and fails on TLS 1.3,
        // and the static FFmpeg (build_deps.sh) has no TLS at all, so tunnel
        // through Windows TLS and keep RTMP itself on plain loopback.
        std::unique_ptr<TlsRelay> relay;
        std::string tc_url;
        if (url.rfind("rtmps://", 0) == 0) {
            std::string rest = url.substr(8);
            size_t slash = rest.find('/');
            std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
            std::string path = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
            std::string host = hostport;
            int port = 443;
            size_t colon = hostport.rfind(':');
            if (colon != std::string::npos) {
                host = hostport.substr(0, colon);
                port = std::atoi(hostport.c_str() + colon + 1);
            }
            std::string app = path.substr(0, path.find('/'));
            relay = std::make_unique<TlsRelay>();
            std::string relay_error;
            if (host.empty() || port <= 0 || !relay->start(host, port, relay_error)) {
                fail(d, relay_error.empty() ? "One or more broadcast destinations are invalid or duplicated." : relay_error);
                running_workers--;
                return;
            }
            url = "rtmp://127.0.0.1:" + std::to_string(relay->local_port()) + "/" + path;
            tc_url = "rtmps://" + hostport + "/" + app;
            if (uses_librtmp()) url += " tcUrl=" + tc_url;
        }
        if (uses_librtmp() && (url.rfind("rtmp", 0) == 0)) url += " timeout=10";
        AVFormatContext* oc = nullptr;
        bool header = false;
        int ret = avformat_alloc_output_context2(&oc, nullptr, "flv", nullptr);
        if (ret < 0 || !oc) {
            fail(d, "Could not create the broadcast output.");
            running_workers--;
            return;
        }
        AVStream* vs = avformat_new_stream(oc, nullptr);
        avcodec_parameters_from_context(vs->codecpar, venc);
        vs->time_base = AVRational{1, 1000};
        AVStream* as = nullptr;
        if (aenc) {
            as = avformat_new_stream(oc, nullptr);
            avcodec_parameters_from_context(as->codecpar, aenc);
            as->time_base = AVRational{1, 1000};
        }
        oc->interrupt_callback.callback = &Impl::interrupt_cb;
        oc->interrupt_callback.opaque = &d;
        AVDictionary* options = nullptr;
        if (!uses_librtmp()) {
            av_dict_set(&options, "rw_timeout", "10000000", 0);
            if (!tc_url.empty()) av_dict_set(&options, "rtmp_tcurl", tc_url.c_str(), 0);
        }
        ret = avio_open2(&oc->pb, url.c_str(), AVIO_FLAG_WRITE, &oc->interrupt_callback, &options);
        av_dict_free(&options);
        if (ret < 0) {
            std::string why = relay ? relay->error() : std::string();
            if (why.empty())
                why = ret == AVERROR_UNKNOWN ? "Could not connect to the broadcast destination. Check the server address and key."
                                             : "Could not connect to the broadcast destination (" + av_error_text(ret) + ").";
            fail(d, why);
        } else if ((ret = avformat_write_header(oc, nullptr)) < 0) {
            fail(d, "Broadcast server rejected the stream header (" + av_error_text(ret) + ").");
        } else {
            header = true;
            {
                std::lock_guard<std::mutex> lock(d.mutex);
                d.status.state = BroadcastDestinationStatus::State::Live;
                logf("broadcast: %s live", d.name.c_str());
                d.live_since = Clock::now();
                d.rate_window = d.live_since;
                d.rate_bytes = 0;
            }
            bool have_offset = false;
            int64_t offset_ms = 0;
            for (;;) {
                QueuedPacket q;
                {
                    std::unique_lock<std::mutex> lock(d.mutex);
                    d.cv.wait_for(lock, std::chrono::milliseconds(200),
                                  [&] { return !d.queue.empty() || d.eof || d.abort.load(); });
                    if (d.abort.load()) break;
                    if (d.queue.empty()) {
                        if (d.eof) break;
                        continue;
                    }
                    q = d.queue.front();
                    d.queue.pop_front();
                    d.queued_bytes -= (size_t)q.packet->size;
                }
                AVPacket* p = q.packet;
                int64_t dts_ms = av_rescale_q(p->dts, q.time_base, AVRational{1, 1000});
                int64_t pts_ms = av_rescale_q(p->pts, q.time_base, AVRational{1, 1000});
                if (!have_offset) {
                    offset_ms = dts_ms;   // first packet is a video keyframe
                    have_offset = true;
                }
                dts_ms -= offset_ms;
                pts_ms -= offset_ms;
                if (dts_ms < 0) {   // audio from before this destination's first keyframe
                    av_packet_free(&p);
                    continue;
                }
                p->dts = dts_ms;
                p->pts = std::max(pts_ms, dts_ms);
                p->duration = av_rescale_q(p->duration, q.time_base, AVRational{1, 1000});
                p->stream_index = q.video ? 0 : 1;
                if (!q.video && !as) {
                    av_packet_free(&p);
                    continue;
                }
                size_t size = (size_t)p->size;
                ret = av_interleaved_write_frame(oc, p);
                av_packet_free(&p);
                if (ret < 0) {
                    if (!d.eof) fail(d, "Broadcast connection failed (" + av_error_text(ret) + ").");
                    break;
                }
                std::lock_guard<std::mutex> lock(d.mutex);
                d.status.bytes += size;
                d.rate_bytes += size;
                auto now = Clock::now();
                double window = std::chrono::duration<double>(now - d.rate_window).count();
                if (window >= 1.0) {
                    d.status.kbps = d.rate_bytes * 8.0 / 1000.0 / window;
                    d.rate_window = now;
                    d.rate_bytes = 0;
                }
            }
        }
        if (header) av_write_trailer(oc);
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        if (relay) relay->stop();
        {
            std::lock_guard<std::mutex> lock(d.mutex);
            if (d.status.state != BroadcastDestinationStatus::State::Failed)
                d.status.state = BroadcastDestinationStatus::State::Stopped;
            d.accepting = false;
            for (auto& q : d.queue) av_packet_free(&q.packet);
            d.queue.clear();
            d.queued_bytes = 0;
        }
        running_workers--;
    }

    void release() {
        if (venc) avcodec_free_context(&venc);
        if (aenc) avcodec_free_context(&aenc);
        if (sws) {
            sws_freeContext(sws);
            sws = nullptr;
        }
        std::lock_guard<std::mutex> lock(in_mutex);
        for (auto& i : input) if (i.frame) av_frame_free(&i.frame);
        input.clear();
        queued_video = 0;
    }
};

Broadcaster::Broadcaster() : impl_(new Impl) {
    impl_->owner = this;
}

Broadcaster::~Broadcaster() {
    stop();
    wait();
}

bool Broadcaster::start(const std::vector<BroadcastTarget>& targets, const BroadcastQuality& quality,
                        bool with_audio, int audio_rate, std::string& error) {
    Impl& s = *impl_;
    if (accepting_.load()) {
        error = "A live broadcast is already running.";
        return false;
    }
    if (s.running_workers.load() > 0) {
        error = "Could not start the live encoder. Stop the existing broadcast first.";
        return false;
    }
    wait();
    if (targets.empty() || targets.size() > 5) {
        error = "Choose between one and five destinations and valid broadcast quality.";
        return false;
    }
    if (quality.fps != 30 && quality.fps != 60) {
        error = "Choose between one and five destinations and valid broadcast quality.";
        return false;
    }
    for (size_t i = 0; i < targets.size(); i++) {
        const std::string& u = targets[i].url;
        if (u.rfind("rtmp://", 0) != 0 && u.rfind("rtmps://", 0) != 0) {
            error = "One or more broadcast destinations are invalid or duplicated.";
            return false;
        }
        for (size_t j = 0; j < i; j++)
            if (targets[j].url == u) {
                error = "One or more broadcast destinations are invalid or duplicated.";
                return false;
            }
    }

    s.quality = quality;
    s.quality.video_kbps = std::clamp(quality.video_kbps, 500, 20000);
    s.quality.audio_kbps = std::clamp(quality.audio_kbps, 64, 320);
    s.with_audio = with_audio;
    s.audio_rate = audio_rate > 0 ? audio_rate : 48000;
    s.video_started = false;
    s.audio_aligned = false;
    s.audio_next = 0;
    s.fifo.clear();
    s.last_out_pts = -1;
    s.last_key_pts = -1;
    s.src_w = s.src_h = 0;
    s.src_fmt = -1;
    s.ready = false;
    s.encoder_failed = false;
    s.frames_in = 0;
    s.frames_encoded = 0;
    s.frames_dropped = 0;
    s.out_w = s.out_h = 0;
    {
        std::lock_guard<std::mutex> lock(s.status_mutex);
        s.encoder_name.clear();
        s.error.clear();
        s.message = "Starting live broadcast...";
        s.started = Clock::now();
    }
    if (with_audio && !s.open_audio_encoder()) {
        error = "Could not configure broadcast audio stream.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s.in_mutex);
        s.input_closed = false;
    }

    s.dests.clear();
    for (const auto& t : targets) {
        auto d = std::make_unique<Destination>();
        d->name = t.name;
        d->url = t.url;
        d->status.name = t.name;
        s.dests.push_back(std::move(d));
    }
    s.stopping = false;
    s.running_workers = 1 + (int)s.dests.size();
    s.encoder_thread = std::thread([&s] { s.run_encoder(); });
    for (auto& d : s.dests) {
        Destination* raw = d.get();
        d->thread = std::thread([&s, raw] { s.run_destination(*raw); });
    }
    accepting_.store(true);
    return true;
}

void Broadcaster::stop(const std::string& reason) {
    Impl& s = *impl_;
    bool was = accepting_.exchange(false);
    if (!was && s.running_workers.load() == 0) return;
    s.stopping = true;
    {
        std::lock_guard<std::mutex> lock(s.status_mutex);
        s.message = reason.empty() ? "Live broadcast stopped." : reason;
    }
    {
        std::lock_guard<std::mutex> lock(s.in_mutex);
        s.input_closed = true;
    }
    s.in_cv.notify_all();
}

void Broadcaster::wait() {
    Impl& s = *impl_;
    if (s.encoder_thread.joinable()) s.encoder_thread.join();
    for (auto& d : s.dests) {
        // Destinations still stuck after the encoder ended are cut after 3 s
        // by the interrupt callback (or librtmp's own 10 s timeout).
        if (d->thread.joinable()) d->thread.join();
    }
    s.release();
    s.stopping = false;
}

BroadcastStatus Broadcaster::status() const {
    const Impl& s = *impl_;
    BroadcastStatus st;
    st.active = accepting_.load();
    st.stopping = s.running_workers.load() > 0 && !st.active;
    {
        std::lock_guard<std::mutex> lock(s.status_mutex);
        st.encoder = s.encoder_name;
        st.error = s.error;
        st.message = s.message;
        if (st.active) st.seconds = std::chrono::duration<double>(Clock::now() - s.started).count();
    }
    st.frames_in = s.frames_in.load();
    st.frames_encoded = s.frames_encoded.load();
    st.frames_dropped = s.frames_dropped.load();
    st.width = s.out_w;
    st.height = s.out_h;
    st.fps = s.quality.fps;
    for (const auto& d : s.dests) {
        std::lock_guard<std::mutex> lock(d->mutex);
        BroadcastDestinationStatus ds = d->status;
        if (ds.state == BroadcastDestinationStatus::State::Live)
            ds.live_seconds = std::chrono::duration<double>(Clock::now() - d->live_since).count();
        st.destinations.push_back(ds);
    }
    return st;
}

void Broadcaster::push_frame(const AVFrame* frame, uint32_t pts90k, std::chrono::steady_clock::time_point arrival) {
    if (!accepting_.load(std::memory_order_relaxed) || !frame) return;
    Impl& s = *impl_;
    s.frames_in++;
    std::lock_guard<std::mutex> lock(s.in_mutex);
    if (s.input_closed) return;
    if (s.queued_video >= 6) {   // encoder behind by ~100 ms: drop this frame
        s.frames_dropped++;
        return;
    }
    Impl::Input item;
    item.frame = av_frame_clone(frame);
    if (!item.frame) return;
    item.pts = pts90k;
    item.arrival = arrival;
    s.input.push_back(std::move(item));
    s.queued_video++;
    s.in_cv.notify_one();
}

void Broadcaster::on_audio(const float* stereo, size_t frames, int) {
    if (!accepting_.load(std::memory_order_relaxed)) return;
    Impl& s = *impl_;
    if (!s.with_audio) return;
    Impl::Input item;
    item.arrival = Clock::now();
    item.samples.assign(stereo, stereo + frames * 2);
    std::lock_guard<std::mutex> lock(s.in_mutex);
    if (s.input_closed || s.input.size() > 2000) return;
    s.input.push_back(std::move(item));
    s.in_cv.notify_one();
}

} // namespace glint
