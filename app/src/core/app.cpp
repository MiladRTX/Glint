#include "app.h"
#include "h264_util.h"
#include "log.h"
#include "slot_timeline.h"
#include "winutil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace glint {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kQueueRelease = 180;        // ~3 s at 60 fps: release regardless of pacing
constexpr size_t kLatenessKeep = 600;        // ~10 s of frames
constexpr double kLateMs = 2.0;
constexpr double kHoldSlewMsPerS = 40.0;
constexpr double kHoldJumpMs = 120.0;

double ms(Clock::duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

struct QueuedFrame {
    std::vector<uint8_t> data;
    uint32_t pts = 0;
    Clock::time_point arrival;
    Clock::time_point ideal;   // arrival minus lateness
};

}  // namespace

struct App::Impl {
    // Settings.
    std::atomic<int> latency{0};
    std::atomic<bool> video_sync{true};
    std::atomic<int> av_offset{0};
    std::atomic<bool> gpu_decode{true};
    std::atomic<bool> gpu_convert{true};
    std::atomic<unsigned> decoder_gen{0};

    LocalRecorder recorder;   // declared before audio: the audio sink feeds it
    AudioPlayer audio;

    // Session state shown to the UI, under state_m.
    mutable std::mutex state_m;
    std::string url;
    std::string error_text;
    std::string media_dir;
    std::string decoder_desc;
    StreamConfig config;
    StreamStatus status;
    std::shared_ptr<const Picture> latest;
    uint64_t latest_seq = 0;
    std::function<void()> notify;

    std::atomic<uint64_t> seq_counter{0};   // never reset: sequence numbers stay unique
    std::atomic<uint64_t> decoded{0};
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic_bool stop_flag{false};      // stop() asked
    std::atomic_bool workers_stop{false};   // decode and status threads end
    std::thread session;

    // Received frames waiting to be decoded.
    std::mutex q_m;
    std::deque<QueuedFrame> queue;
    HANDLE wake = nullptr;
    HANDLE timer = nullptr;

    // Pacing and receive statistics, under pace_m.
    mutable std::mutex pace_m;
    Clock::time_point epoch;
    PtsUnwrap unwrap;
    bool have_base = false;
    double base = 0.0;          // smallest arrival-minus-timestamp seen, in s
    double last_t = 0.0;
    double peak_late = 0.0;     // decaying peak of lateness, ms
    std::vector<float> lateness;   // ring of the last kLatenessKeep values
    size_t late_pos = 0, late_count = 0;
    std::deque<std::pair<Clock::time_point, size_t>> rx;
    double hold_ms = 0.0;
    std::atomic<float> hold_shown{0.0f};
    std::atomic<uint64_t> late{0};

    // Timing for the periodic stats line, under pace_m; reset after each line.
    uint64_t t_frames = 0;
    double t_decode_sum = 0.0, t_decode_max = 0.0;   // decode call, ms
    double t_total_sum = 0.0, t_total_max = 0.0;     // arrival to published, ms
    size_t t_queue_max = 0;
    std::vector<float> t_late;                       // lateness since the last line
    std::vector<float> t_size;                       // access-unit sizes since the last line, KB
    std::vector<float> t_qp;                         // first-slice QP of each frame
    SliceQp slice_qp;                                // set before the stream starts

    // Status thread wake-up.
    std::mutex cmd_m;
    std::condition_variable cmd_cv;
    bool keyframe_wanted = false;

    Impl()
    {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);   // older Windows
        lateness.assign(kLatenessKeep, 0.0f);
    }

    ~Impl()
    {
        if (wake) CloseHandle(wake);
        if (timer) CloseHandle(timer);
    }

    void set_error(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(state_m);
        error_text = text;
    }

    void request_keyframe()
    {
        std::lock_guard<std::mutex> lock(cmd_m);
        keyframe_wanted = true;
        cmd_cv.notify_all();
    }

    bool stopping() const { return stop_flag.load() || workers_stop.load(); }

    // Interruptible sleep for the session threads.
    void nap(int total_ms)
    {
        std::unique_lock<std::mutex> lock(cmd_m);
        cmd_cv.wait_for(lock, std::chrono::milliseconds(total_ms), [&] { return stopping(); });
    }

    // --- pacing (network thread) -------------------------------------------------

    double hold_target(double late_ms, double dt)
    {
        peak_late = std::max(late_ms, peak_late * std::exp(-dt * 0.0693));   // half-life ~10 s
        int mode = latency.load();
        if (mode == 1) {
            std::vector<float> v(lateness.begin(), lateness.begin() + (ptrdiff_t)late_count);
            if (v.empty()) return 8.0;
            size_t k = (size_t)(0.99 * (double)(v.size() - 1));
            std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
            return std::clamp((double)v[k] + 4.0, 8.0, 120.0);
        }
        if (mode == 2) {
            double h = peak_late + 8.0;
            double playout = audio.playout_ms();
            if (video_sync.load() && playout >= 0) h = std::max(h, playout + av_offset.load());
            return std::clamp(h, 0.0, 800.0);
        }
        return 0.0;
    }

    void on_frame(const uint8_t* data, size_t len, uint32_t pts)
    {
        Clock::time_point now = Clock::now();
        recorder.push_video(data, len, pts, now);

        int qp = slice_qp.qp(data, len);
        QueuedFrame f;
        f.data.assign(data, data + len);
        f.pts = pts;
        f.arrival = now;
        {
            std::lock_guard<std::mutex> lock(pace_m);
            double t = std::chrono::duration<double>(now - epoch).count();
            double offset = t - (double)unwrap(pts) / 90000.0;
            double dt = have_base ? std::max(0.0, t - last_t) : 0.0;
            // The baseline may rise a couple of ms per second so clock drift doesn't build up.
            base = have_base ? std::min(base + 0.002 * dt, offset) : offset;
            have_base = true;
            last_t = t;
            double late_ms = (offset - base) * 1000.0;
            lateness[late_pos] = (float)late_ms;
            t_late.push_back((float)late_ms);
            t_size.push_back((float)len / 1000.0f);
            if (qp >= 0) t_qp.push_back((float)qp);
            late_pos = (late_pos + 1) % kLatenessKeep;
            late_count = std::min(late_count + 1, kLatenessKeep);
            f.ideal = now - std::chrono::microseconds((int64_t)(late_ms * 1000.0));

            double target = hold_target(late_ms, dt);
            if (latency.load() == 0) hold_ms = 0.0;
            else if (std::fabs(target - hold_ms) > kHoldJumpMs) hold_ms = target;
            else hold_ms += std::clamp(target - hold_ms, -kHoldSlewMsPerS * dt, kHoldSlewMsPerS * dt);
            hold_shown.store((float)hold_ms);

            rx.emplace_back(now, len);
            while (!rx.empty() && now - rx.front().first > std::chrono::seconds(2)) rx.pop_front();
        }
        size_t depth;
        {
            std::lock_guard<std::mutex> lock(q_m);
            queue.push_back(std::move(f));
            depth = queue.size();
        }
        {
            std::lock_guard<std::mutex> lock(pace_m);
            t_queue_max = std::max(t_queue_max, depth);
        }
        SetEvent(wake);
    }

    // --- decoding (decode thread) --------------------------------------------------

    // Waits until the oldest frame is due (or the session ends). False on stop.
    bool next_frame(QueuedFrame& out, bool& paced, Clock::time_point& due)
    {
        for (;;) {
            if (stopping()) return false;
            bool timed = false;
            {
                std::lock_guard<std::mutex> lock(q_m);
                if (!queue.empty()) {
                    Clock::time_point now = Clock::now();
                    paced = latency.load() != 0 && queue.size() <= kQueueRelease;
                    due = queue.front().ideal + std::chrono::microseconds((int64_t)(hold_shown.load() * 1000.0f));
                    if (!paced || now >= due) {
                        out = std::move(queue.front());
                        queue.pop_front();
                        return true;
                    }
                    // A high-resolution timer: condition-variable timeouts overshoot by milliseconds.
                    LARGE_INTEGER rel;
                    int64_t wait_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(due - now).count() / 100;
                    rel.QuadPart = -std::max<int64_t>(1, wait_100ns);
                    SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE);
                    timed = true;
                }
            }
            // A new frame, a mode change or stop signals `wake`.
            if (timed) {
                HANDLE both[2] = {wake, timer};
                WaitForMultipleObjects(2, both, FALSE, INFINITE);
            } else {
                WaitForSingleObject(wake, INFINITE);
            }
        }
    }

    void decode_loop(H264Decoder* dec, unsigned gen, std::vector<uint8_t> param_sets)
    {
        bool need_idr = true;
        bool cpu_forced = false;
        int errors_logged = 0;
        std::function<void()> wake_ui;
        {
            std::lock_guard<std::mutex> lock(state_m);
            wake_ui = notify;
        }
        QueuedFrame f;
        std::vector<uint8_t> au;
        for (;;) {
            bool paced = false;
            Clock::time_point due;
            if (!next_frame(f, paced, due)) break;
            if (paced && ms(Clock::now() - due) > kLateMs) late++;

            unsigned want_gen = decoder_gen.load();
            if (want_gen != gen) {
                // The user switched GPU/CPU decoding: reopen, resume at the next keyframe.
                gen = want_gen;
                cpu_forced = false;
                std::string err;
                if (!dec->open(err, gpu_decode.load())) logf("video: decoder reopen failed: %s", err.c_str());
                else logf("video: decoder switched to %s", dec->description().c_str());
                need_idr = true;
                request_keyframe();
                std::lock_guard<std::mutex> lock(state_m);
                decoder_desc = dec->description();
            }
            bool idr = has_idr(f.data.data(), f.data.size());
            if (need_idr) {
                if (!idr) continue;   // no smeared pictures before the first keyframe
                need_idr = false;
            }
            const uint8_t* data = f.data.data();
            size_t size = f.data.size();
            if (idr && !param_sets.empty()) {
                au.assign(param_sets.begin(), param_sets.end());
                au.insert(au.end(), f.data.begin(), f.data.end());
                data = au.data();
                size = au.size();
            }
            dec->set_output(gpu_convert.load() ? H264Decoder::Output::Yuv : H264Decoder::Output::Rgba);
            auto pic = std::make_shared<Picture>();
            std::string err;
            Clock::time_point t0 = Clock::now();
            bool ok = dec->decode(data, size, f.pts, *pic, err);
            Clock::time_point t1 = Clock::now();
            if (!ok && !err.empty() && errors_logged < 20) {
                errors_logged++;
                logf("video: %s", err.c_str());
            }
            if (!cpu_forced && dec->needs_cpu_fallback()) {
                cpu_forced = true;
                std::string open_err;
                dec->open(open_err, false);
                need_idr = true;
                request_keyframe();
                logf("video: GPU decoding failed 3 times in a row; decoding on the CPU from now on");
                std::lock_guard<std::mutex> lock(state_m);
                decoder_desc = "CPU (repeated GPU errors)";
            }
            if (!ok) continue;

            RecorderInfo::State rs = recorder.state();
            if (rs == RecorderInfo::State::Armed || rs == RecorderInfo::State::Recording) {
                RecorderInfo ri = recorder.info();
                if (ri.width != pic->width || ri.height != pic->height)
                    recorder.stop("the picture size changed");
            }
            {
                std::lock_guard<std::mutex> lock(state_m);
                latest = std::move(pic);
                latest_seq = ++seq_counter;
                if (!cpu_forced && decoder_desc != dec->description()) decoder_desc = dec->description();
            }
            decoded++;
            if (wake_ui) wake_ui();
            {
                std::lock_guard<std::mutex> lock(pace_m);
                double dec_ms = ms(t1 - t0), total_ms = ms(Clock::now() - f.arrival);
                t_frames++;
                t_decode_sum += dec_ms;
                t_decode_max = std::max(t_decode_max, dec_ms);
                t_total_sum += total_ms;
                t_total_max = std::max(t_total_max, total_ms);
            }
        }
    }

    // --- status and keyframe requests (status thread) ------------------------------

    // One line of timing every 10 s, for tuning latency from the log.
    void log_stats(double span_s)
    {
        uint64_t n;
        double dec_avg, dec_max, tot_avg, tot_max;
        size_t qmax;
        std::vector<float> lv, sv, qv;
        {
            std::lock_guard<std::mutex> lock(pace_m);
            sv.swap(t_size);
            qv.swap(t_qp);
            n = t_frames;
            dec_avg = n ? t_decode_sum / n : 0.0;
            dec_max = t_decode_max;
            tot_avg = n ? t_total_sum / n : 0.0;
            tot_max = t_total_max;
            qmax = t_queue_max;
            lv.swap(t_late);
            t_frames = 0;
            t_decode_sum = t_decode_max = t_total_sum = t_total_max = 0.0;
            t_queue_max = 0;
        }
        auto pct_of = [](std::vector<float>& v, double p) {
            if (v.empty()) return 0.0;
            size_t k = (size_t)(p * (double)(v.size() - 1));
            std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
            return (double)v[k];
        };
        auto pct = [&](double p) { return pct_of(lv, p); };
        double l50 = pct(0.5), l95 = pct(0.95), l99 = pct(0.99), lmax = pct(1.0);
        // Frames bunched just under the largest size mean the encoder's per-frame cap is
        // what limits quality in motion.
        double s50 = pct_of(sv, 0.5), s90 = pct_of(sv, 0.9), s99 = pct_of(sv, 0.99), smax = pct_of(sv, 1.0);
        size_t over12 = (size_t)std::count_if(lv.begin(), lv.end(), [](float x) { return x > 12.0f; });
        AudioStats a = audio.stats();
        logf("stats: rx %.1f fps, late p50 %.1f p95 %.1f p99 %.1f max %.1f ms (%zu over 12 ms); "
             "decode avg %.2f max %.2f ms; arrival->ready avg %.2f max %.2f ms; queue max %zu; hold %.1f ms, "
             "%llu late | audio target %.0f buffered %.0f playout %.0f ms, ratio %.4f, underruns %llu, "
             "max packet gap %.0f ms",
             n / span_s, l50, l95, l99, lmax, over12, dec_avg, dec_max, tot_avg, tot_max, qmax,
             hold_shown.load(), (unsigned long long)late.load(), a.target_ms, a.buffered_ms, a.playout_ms,
             a.ratio, (unsigned long long)a.underruns, a.max_packet_gap_ms);
        double qmin = pct_of(qv, 0.0);
        size_t at_floor = (size_t)std::count_if(qv.begin(), qv.end(), [&](float q) { return q <= qmin + 0.5; });
        logf("stats: frame size p50 %.0f p90 %.0f p99 %.0f max %.0f KB; QP min %.0f p50 %.0f p90 %.0f max %.0f "
             "(%zu of %zu frames at the minimum)",
             s50, s90, s99, smax, qmin, pct_of(qv, 0.5), pct_of(qv, 0.9), pct_of(qv, 1.0), at_floor, qv.size());
    }

    void status_loop(std::string base)
    {
        PayloadClient client(base);
        client.set_cancel(&stop_flag);
        Clock::time_point next_poll = Clock::now();
        Clock::time_point last_key = Clock::now();
        Clock::time_point last_stats = Clock::now();
        while (!stopping()) {
            Clock::time_point now = Clock::now();
            if (now - last_stats >= std::chrono::seconds(10)) {
                log_stats(std::chrono::duration<double>(now - last_stats).count());
                last_stats = now;
            }
            if (now >= next_poll) {
                StreamStatus s;
                std::string err;
                if (client.fetch_status(s, err)) {
                    std::lock_guard<std::mutex> lock(state_m);
                    status = s;
                }
                next_poll = Clock::now() + std::chrono::seconds(1);
            }
            bool want = false;
            {
                std::lock_guard<std::mutex> lock(cmd_m);
                want = keyframe_wanted;
                keyframe_wanted = false;
            }
            RecorderInfo::State rs = recorder.state();
            now = Clock::now();
            if (rs == RecorderInfo::State::Armed && now - last_key >= std::chrono::seconds(1)) want = true;
            if (rs == RecorderInfo::State::Recording && now - last_key >= std::chrono::seconds(2)) want = true;
            if (want && !stopping()) {
                std::string err;
                if (!client.post("/video/keyframe", nullptr, err) && !stopping())
                    logf("video: keyframe request failed: %s", err.c_str());
                last_key = Clock::now();
            }
            std::unique_lock<std::mutex> lock(cmd_m);
            cmd_cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping() || keyframe_wanted; });
        }
    }

    // --- the session --------------------------------------------------------------

    void run_session(std::string base)
    {
        PayloadClient client(base);
        client.set_cancel(&stop_flag);
        StreamConfig cfg;
        std::string err;
        logf("connecting to %s", base.c_str());
        if (!client.fetch_config(cfg, err)) {
            if (stop_flag.load()) return;
            set_error("Cannot connect to the console: " + err + ".");
            logf("connect failed: %s", err.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_m);
            config = cfg;
            error_text = "Connected to " + (cfg.platform.empty() ? std::string("the console") : cfg.platform) + ".";
        }
        if (cfg.has_audio)
            logf("stream: %s %dx%d@%d %s, audio %s %dch %d Hz", cfg.platform.c_str(), cfg.width, cfg.height, cfg.fps,
                 cfg.codec.c_str(), cfg.audio_codec.c_str(), cfg.audio_channels, cfg.audio_rate);
        else
            logf("stream: %s %dx%d@%d %s, no audio", cfg.platform.c_str(), cfg.width, cfg.height, cfg.fps,
                 cfg.codec.c_str());

        // SPS and PPS go in front of every keyframe.
        std::vector<uint8_t> sps, pps, param_sets;
        hex_to_bytes(cfg.sps, sps);
        hex_to_bytes(cfg.pps, pps);
        if (valid_param_sets(sps, pps)) {
            static const uint8_t sc[4] = {0, 0, 0, 1};
            param_sets.insert(param_sets.end(), sc, sc + 4);
            param_sets.insert(param_sets.end(), sps.begin(), sps.end());
            param_sets.insert(param_sets.end(), sc, sc + 4);
            param_sets.insert(param_sets.end(), pps.begin(), pps.end());
        } else {
            logf("stream: the console sent no usable SPS/PPS");
        }
        slice_qp.set_params(sps, pps);

        H264Decoder decoder;
        unsigned gen = decoder_gen.load();
        if (!decoder.open(err, gpu_decode.load())) {
            set_error("Video decoder could not start: " + err);
            logf("video: %s", err.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(state_m);
            decoder_desc = decoder.description();
        }
        logf("video: decoding with %s", decoder.description().c_str());
        connected.store(true);

        if (cfg.has_audio) {
            if (audio.start_session(base, cfg)) logf("audio: playing %s", audio.stats().format.c_str());
            else logf("audio: not started (%s) %s", audio.stats().state.c_str(), audio.last_error().c_str());
        } else {
            logf("audio: the console sends no sound");
        }

        std::string key_err;
        if (!client.post("/video/keyframe", nullptr, key_err) && !stop_flag.load())
            logf("video: keyframe request failed: %s", key_err.c_str());

        workers_stop.store(false);
        std::thread decode_thread([this, &decoder, gen, param_sets] { decode_loop(&decoder, gen, param_sets); });
        std::thread status_thread([this, base] { status_loop(base); });

        bool ok = client.stream_frames([this](const uint8_t* d, size_t n, uint32_t pts) {
            on_frame(d, n, pts);
            return !stop_flag.load();
        }, stop_flag, err);

        if (!stop_flag.load()) {
            // The stream ended on its own: report why and wait for the user.
            connected.store(false);
            recorder.stop("the stream ended");
            audio.stop_session();
            bool taken = !ok && err == kTakenOver;
            if (ok) {
                // A normal close: another viewer, or the console stopped streaming?
                nap(300);
                StreamStatus s;
                std::string serr;
                if (!stop_flag.load() && client.fetch_status(s, serr) && s.viewers > 0) taken = true;
            }
            std::string text;
            if (taken)
                text = "Video stopped: Glint is open in a browser or on another PC, which took over the stream.";
            else if (ok)
                text = "Video stream ended: the console closed it.";
            else
                text = "Video stream lost: " + err + ".";
            set_error(text);
            logf("stream ended: %s", text.c_str());
        }
        workers_stop.store(true);
        SetEvent(wake);
        cmd_cv.notify_all();
        decode_thread.join();
        status_thread.join();
        hold_shown.store(0.0f);
        {
            std::lock_guard<std::mutex> lock(q_m);
            queue.clear();
        }
    }
};

App::App() : impl_(new Impl)
{
    impl_->media_dir = default_media_dir();
    Impl* d = impl_.get();
    impl_->audio.set_sink([d](const float* stereo, size_t frames, int rate, Clock::time_point arrival) {
        d->recorder.push_audio(stereo, frames, rate, arrival);
    });
    set_latency(Latency::Lowest);
}

App::~App()
{
    stop();
    impl_->audio.set_sink(nullptr);
    impl_->recorder.wait();
}

void App::start(const std::string& url)
{
    stop();
    Impl& d = *impl_;
    {
        std::lock_guard<std::mutex> lock(d.state_m);
        d.url = url;
        d.error_text.clear();
        d.config = StreamConfig();
        d.status = StreamStatus();
        d.latest.reset();
    }
    {
        std::lock_guard<std::mutex> lock(d.pace_m);
        d.epoch = Clock::now();
        d.unwrap = PtsUnwrap();
        d.have_base = false;
        d.peak_late = 0.0;
        d.late_pos = d.late_count = 0;
        d.rx.clear();
        d.hold_ms = 0.0;
    }
    {
        std::lock_guard<std::mutex> lock(d.cmd_m);
        d.keyframe_wanted = false;
    }
    d.hold_shown.store(0.0f);
    d.late.store(0);
    d.decoded.store(0);
    d.stop_flag.store(false);
    d.workers_stop.store(false);
    d.running.store(true);
    std::string base = url;
    d.session = std::thread([&d, base] { d.run_session(base); });
}

void App::stop()
{
    Impl& d = *impl_;
    if (!d.running.load() && !d.session.joinable()) return;
    d.recorder.stop("the source disconnected");
    d.stop_flag.store(true);
    SetEvent(d.wake);
    d.cmd_cv.notify_all();
    if (d.session.joinable()) d.session.join();
    d.audio.stop_session();
    d.connected.store(false);
    d.running.store(false);
    d.hold_shown.store(0.0f);
    logf("disconnected");
}

bool App::running() const
{
    return impl_->running.load();
}

bool App::connected() const
{
    return impl_->connected.load();
}

std::string App::url() const
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    return impl_->url;
}

std::string App::error() const
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    return impl_->error_text;
}

StreamConfig App::config() const
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    return impl_->config;
}

StreamStatus App::status() const
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    return impl_->status;
}

bool App::post(const std::string& path, std::string& error)
{
    std::string base = url();
    if (base.empty()) {
        error = "No console address is set.";
        return false;
    }
    PayloadClient client(base);
    return client.post(path, nullptr, error);
}

void App::set_frame_notify(std::function<void()> notify)
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    impl_->notify = std::move(notify);
}

std::shared_ptr<const Picture> App::take_frame(uint64_t& last_seen)
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    if (!impl_->latest || impl_->latest_seq == last_seen) return nullptr;
    last_seen = impl_->latest_seq;
    return impl_->latest;
}

uint64_t App::frame_count() const
{
    return impl_->decoded.load();
}

void App::received_rate(float& fps, float& kbps) const
{
    Impl& d = *impl_;
    fps = kbps = 0.0f;
    std::lock_guard<std::mutex> lock(d.pace_m);
    Clock::time_point now = Clock::now();
    if (d.rx.size() < 2 || now - d.rx.back().first > std::chrono::seconds(2)) return;
    auto first = d.rx.begin();
    while (first != d.rx.end() && now - first->first > std::chrono::seconds(2)) ++first;
    if (first == d.rx.end()) return;
    double span = std::chrono::duration<double>(d.rx.back().first - first->first).count();
    if (span < 0.5) return;
    size_t frames = 0, bytes = 0;
    for (auto it = first + 1; it != d.rx.end(); ++it) {
        frames++;
        bytes += it->second;
    }
    fps = (float)(frames / span);
    kbps = (float)(bytes * 8.0 / 1000.0 / span);
}

std::vector<float> App::recent_lateness(size_t count) const
{
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.pace_m);
    size_t n = std::min(count, d.late_count);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; i++) {
        size_t idx = (d.late_pos + kLatenessKeep - n + i) % kLatenessKeep;
        out[i] = d.lateness[idx];
    }
    return out;
}

uint64_t App::late_frames() const
{
    return impl_->late.load();
}

float App::video_hold_ms() const
{
    return impl_->connected.load() ? impl_->hold_shown.load() : 0.0f;
}

void App::set_latency(Latency latency)
{
    int mode = std::clamp((int)latency, 0, 2);
    impl_->latency.store(mode);
    static const int kFloor[3] = {40, 60, 120};
    impl_->audio.set_auto_floor_ms(kFloor[mode]);
    if (impl_->wake) SetEvent(impl_->wake);   // re-time a frame that is waiting
}

App::Latency App::latency() const
{
    return (Latency)impl_->latency.load();
}

void App::set_video_sync(bool on)
{
    impl_->video_sync.store(on);
}

bool App::video_sync() const
{
    return impl_->video_sync.load();
}

void App::set_av_offset_ms(int ms)
{
    impl_->av_offset.store(std::clamp(ms, -200, 200));
}

int App::av_offset_ms() const
{
    return impl_->av_offset.load();
}

void App::set_gpu_decode(bool on)
{
    impl_->gpu_decode.store(on);
    impl_->decoder_gen++;   // the decode thread reopens at the next frame
}

bool App::gpu_decode() const
{
    return impl_->gpu_decode.load();
}

void App::set_gpu_convert(bool on)
{
    impl_->gpu_convert.store(on);
}

bool App::gpu_convert() const
{
    return impl_->gpu_convert.load();
}

std::string App::decoder_description() const
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    return impl_->decoder_desc;
}

AudioPlayer& App::audio()
{
    return impl_->audio;
}

std::string App::media_dir() const
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    return impl_->media_dir;
}

void App::set_media_dir(const std::string& dir)
{
    std::lock_guard<std::mutex> lock(impl_->state_m);
    impl_->media_dir = dir;
}

bool App::take_screenshot(std::string& saved_path, std::string& error)
{
    std::shared_ptr<const Picture> pic;
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(impl_->state_m);
        pic = impl_->latest;
        dir = impl_->media_dir;
    }
    if (!pic || pic->width <= 0) {
        error = "There is no picture yet to save.";
        return false;
    }
    std::vector<uint8_t> rgba;
    if (!picture_to_rgba(*pic, rgba)) {
        error = "The picture could not be converted.";
        return false;
    }
    return save_screenshot(dir, rgba, pic->width, pic->height, saved_path, error);
}

bool App::start_local_recording(std::string& error)
{
    Impl& d = *impl_;
    if (!d.connected.load()) {
        error = "Connect to a console before recording.";
        return false;
    }
    StreamConfig cfg = config();
    bool with_audio = d.audio.active();
    if (!d.recorder.start(media_dir(), cfg, with_audio, cfg.audio_rate, error)) return false;
    d.request_keyframe();
    return true;
}

void App::stop_local_recording()
{
    impl_->recorder.stop(std::string());
}

RecorderInfo App::recording_info() const
{
    return impl_->recorder.info();
}

}
