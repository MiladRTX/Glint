#include "app.h"

#include "decoder.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <iterator>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#    define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#  endif
#endif

namespace glint {

namespace {
// Sleeps until a paced frame is due. Timed condition-variable waits overshoot
// by 2-15 ms here (MinGW, even at 1 ms timer resolution), which shows paced
// frames late; Windows' high-resolution waitable timer is within ~0.5 ms.
class PreciseSleeper {
public:
    PreciseSleeper() {
#ifdef _WIN32
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    }
    ~PreciseSleeper() {
#ifdef _WIN32
        if (timer_) CloseHandle(timer_);
#endif
    }
    void sleep_until(std::chrono::steady_clock::time_point until) {
        auto left = until - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) return;
#ifdef _WIN32
        if (timer_) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100);
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer_, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_until(until);
    }

private:
#ifdef _WIN32
    HANDLE timer_ = nullptr;
#endif
};
} // namespace

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static std::vector<uint8_t> hex_decode(const std::string& text) {
    std::vector<uint8_t> out;
    if (text.size() % 2) return out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        int high = hex_value(text[i]);
        int low = hex_value(text[i + 1]);
        if (high < 0 || low < 0) return {};
        out.push_back((uint8_t)((high << 4) | low));
    }
    return out;
}

static bool has_idr(const uint8_t* data, size_t len) {
    for (size_t i = 0; i + 4 < len; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
            (data[i + 3] & 0x1f) == 5)
            return true;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
            data[i + 3] == 1 && i + 4 < len && (data[i + 4] & 0x1f) == 5)
            return true;
    }
    return false;
}

static void append_nal(std::vector<uint8_t>& out, const std::vector<uint8_t>& nal) {
    static const uint8_t start[] = {0, 0, 0, 1};
    out.insert(out.end(), start, start + 4);
    out.insert(out.end(), nal.begin(), nal.end());
}

App::~App() {
    stop();
}

void App::start(const std::string& url) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url_ = url;
        error_.clear();
        config_ = StreamConfig{};
        status_ = StreamStatus{};
        frame_.reset();
        frame_width_ = frame_height_ = 0;
        frame_seq_ = 0;
        frames_.store(0);
    }
    rx_fps_.store(0.0f);
    rx_kbps_.store(0.0f);
    rx_last_ms_.store(0);
    late_frames_.store(0);
    {
        std::lock_guard<std::mutex> lock(lateness_mutex_);
        lateness_next_ = lateness_count_ = 0;
    }
    stop_.store(false);
    running_.store(true);
    connected_.store(false);
    worker_ = std::thread(&App::worker, this);
    status_thread_ = std::thread(&App::status_worker, this);
}

void App::stop() {
    stop_.store(true);
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    if (status_thread_.joinable()) status_thread_.join();
    recorder_.stop("Local recording stopped because the source disconnected.");
    broadcaster_.stop("Live broadcast stopped because the console disconnected.");
    audio_.remove_sink(&broadcaster_);
    audio_.remove_sink(&recorder_);
    recorder_.wait();
    audio_.stop();
    connected_.store(false);
}

void App::worker() {
    PayloadClient client(url_);
    std::string error;
    StreamConfig config;
    if (!client.fetch_config(config, error)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Cannot connect: " + error;
        logf("connect %s failed: %s", url_.c_str(), error.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        error_ = "Connected to " + config.platform;
        logf("connected to %s at %s: %dx%d@%d %s, audio %s %dch %d Hz", config.platform.c_str(), url_.c_str(),
             config.width, config.height, config.fps, config.codec.c_str(),
             config.has_audio ? config.audio_codec.c_str() : "none", config.audio_channels, config.audio_rate);
    }

    H264Decoder decoder;
    if (!decoder.open(error, gpu_decode_.load())) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = error;
        return;
    }

    connected_.store(true);
    if (!stop_.load()) {
        if (audio_.start(url_, config)) logf("audio: playing %s", audio_.stats().device.c_str());
        else logf("audio: not started (%s) %s", audio_.stats().state.c_str(), audio_.last_error().c_str());
    }

    std::string ignored;
    client.post("/video/keyframe", nullptr, ignored);

    std::vector<uint8_t> params;
    append_nal(params, hex_decode(config.sps));
    append_nal(params, hex_decode(config.pps));
    if (params.size() == 8) params.clear();

    // Frames are queued on arrival and decoded on a separate thread once
    // arrival + hold has passed, where hold follows the audio playout latency.
    // The network read never waits (the payload resets viewers that fall more
    // than 6 frames behind), and only compressed frames are held.
    struct Pending {
        std::vector<uint8_t> data;   // Annex-B, SPS/PPS prepended on IDR
        uint32_t pts = 0;
        std::chrono::steady_clock::time_point arrival;
        // Balanced/Smooth latency: when this frame would have arrived on a perfect
        // network (earliest-arrival baseline + console timestamp).
        std::chrono::steady_clock::time_point ideal;
    };
    // Video arrival jitter (network thread only).
    bool pace_init = false;
    uint32_t pace_last_raw = 0;
    int64_t pace_unwrapped = 0;
    std::chrono::steady_clock::time_point pace_t0;
    double pace_baseline_ms = 0.0, pace_peak_ms = 30.0, pace_last_arrival_ms = 0.0;
    std::deque<double> pace_window;   // excess of the last 10 s of frames (Balanced)
    std::vector<double> pace_sorted;
    // Received rate over the last 2 s (network thread only).
    struct Received { std::chrono::steady_clock::time_point at; size_t bytes; };
    std::deque<Received> rx_window;
    size_t rx_window_bytes = 0;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Pending> queue;
    bool stream_done = false;
    video_hold_ms_.store(0.0f);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        decoder_desc_ = decoder.description();
    }
    logf("video decoder: %s", decoder.description().c_str());

    std::thread decode_thread([&]() {
        using clock = std::chrono::steady_clock;
        PreciseSleeper sleeper;
        std::string decode_error;
        bool opened_gpu = gpu_decode_.load();
        bool forced_cpu = false;     // GPU kept failing: stay on the CPU until the switch changes
        bool last_pref = opened_gpu;
        bool wait_idr = false;
        std::string last_desc = decoder.description();
        double hold = 0.0;
        auto last_update = clock::now();
        auto update_hold = [&](clock::time_point now) {
            double target = video_hold_target_ms();
            double dt = std::chrono::duration<double>(now - last_update).count();
            last_update = now;
            double diff = target - hold;
            // Jump on start/toggle; otherwise slew faster than the audio
            // controller can move (+-3 % ~ 30 ms/s) so the two stay together.
            if (std::fabs(diff) > 120.0) hold = target;
            else hold += std::clamp(diff, -40.0 * dt, 40.0 * dt);
            video_hold_ms_.store((float)hold);
        };
        for (;;) {
            Pending item;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                for (;;) {
                    if (stop_.load()) return;
                    auto now = clock::now();
                    update_hold(now);
                    if (!queue.empty()) {
                        const Pending& front = queue.front();
                        bool paced = latency_.load() != (int)Latency::Lowest;
                        auto due = (paced ? front.ideal : front.arrival) +
                                   std::chrono::microseconds((long long)(hold * 1000.0));
                        // Never hold more than ~3 s of frames.
                        if (now >= due || queue.size() > 180) {
                            // Arrived after its slot: shown late, a visible hitch.
                            if (paced && now - due > std::chrono::milliseconds(2)) late_frames_.fetch_add(1);
                            break;
                        }
                        // The front's due time cannot change while it waits (new
                        // frames only queue behind it), so sleep without the lock.
                        auto want = std::min(due, now + std::chrono::milliseconds(20));
                        lock.unlock();
                        sleeper.sleep_until(want);
                        lock.lock();
                    } else if (stream_done) {
                        return;
                    } else {
                        queue_cv.wait_for(lock, std::chrono::milliseconds(20));
                    }
                }
                item = std::move(queue.front());
                queue.pop_front();
            }
            // Reopen when the decoder switch changes or the GPU keeps failing.
            bool pref = gpu_decode_.load();
            if (pref != last_pref) {
                forced_cpu = false;
                last_pref = pref;
            }
            if (decoder.needs_cpu_fallback() && !forced_cpu) {
                forced_cpu = true;
                logf("video decoder: GPU decoding failed repeatedly; switching to the CPU");
            }
            bool want_gpu = pref && !forced_cpu;
            if (want_gpu != opened_gpu) {
                decoder.close();
                std::string open_error;
                if (!decoder.open(open_error, want_gpu)) logf("video decoder: reopen failed: %s", open_error.c_str());
                opened_gpu = want_gpu;
                wait_idr = true;
                std::string url = url_;
                std::thread([url] {   // ask for a keyframe so the new decoder can start
                    std::string ignored;
                    PayloadClient(url).post("/video/keyframe", nullptr, ignored);
                }).detach();
            }
            if (wait_idr) {
                if (!has_idr(item.data.data(), item.data.size())) continue;
                wait_idr = false;
            }
            decoder.set_output(gpu_convert_.load() ? H264Decoder::Output::Yuv : H264Decoder::Output::Rgba);

            Picture picture;
            auto arrival = item.arrival;
            uint32_t pts = item.pts;
            auto tap = [this, arrival, pts](const AVFrame* frame) {
                broadcaster_.push_frame(frame, pts, arrival);
            };
            if (decoder.decode(item.data.data(), item.data.size(), item.pts, picture, decode_error, tap)) {
                int width = picture.width, height = picture.height;
                int rw = rec_width_.load();
                if (rw != 0 && (width != rw || height != rec_height_.load())) {
                    rec_width_.store(0);
                    recorder_.stop("Local recording stopped because the resolution changed.");
                }
                auto buffer = std::make_shared<Picture>(std::move(picture));
                std::lock_guard<std::mutex> lock(mutex_);
                frame_ = std::move(buffer);
                frame_width_ = width;
                frame_height_ = height;
                frame_seq_++;
                frames_.fetch_add(1);
                if (frame_notify_) frame_notify_();
                if (decoder.description() != last_desc) {
                    last_desc = decoder.description();
                    decoder_desc_ = last_desc;
                    logf("video decoder: %s", last_desc.c_str());
                }
            }
        }
    });

    client.stream_frames(
        [&](const uint8_t* annexb, size_t len, uint32_t pts) {
            if (stop_.load()) return false;
            Pending item;
            item.pts = pts;
            item.arrival = std::chrono::steady_clock::now();
            {
                // Lateness of this frame against the console clock, relative to
                // the earliest arrival seen (creeping 2 ms/s to follow drift),
                // remembered with a 10 s half-life: that is the jitter buffer.
                if (!pace_init) {
                    pace_init = true;
                    pace_last_raw = pts;
                    pace_unwrapped = 0;
                    pace_t0 = item.arrival;
                    pace_baseline_ms = 0.0;
                    pace_last_arrival_ms = 0.0;
                } else {
                    pace_unwrapped += (int32_t)(pts - pace_last_raw);
                    pace_last_raw = pts;
                }
                double arrival_ms = std::chrono::duration<double, std::milli>(item.arrival - pace_t0).count();
                double media_ms = pace_unwrapped / 90.0;
                double lateness = arrival_ms - media_ms;
                double dt = std::max(0.0, arrival_ms - pace_last_arrival_ms) / 1000.0;
                pace_last_arrival_ms = arrival_ms;
                pace_baseline_ms = std::min(lateness, pace_baseline_ms + 2.0 * dt);
                double excess = lateness - pace_baseline_ms;
                pace_peak_ms = std::max(excess, pace_peak_ms * std::pow(0.5, dt / 10.0));
                video_jitter_ms_.store((float)pace_peak_ms);
                pace_window.push_back(excess);
                if (pace_window.size() > 600) pace_window.pop_front();
                pace_sorted.assign(pace_window.begin(), pace_window.end());
                auto p99 = pace_sorted.begin() + (ptrdiff_t)((pace_sorted.size() - 1) * 99 / 100);
                std::nth_element(pace_sorted.begin(), p99, pace_sorted.end());
                video_jitter_p99_ms_.store((float)*p99);
                {
                    std::lock_guard<std::mutex> lock(lateness_mutex_);
                    lateness_ring_[lateness_next_] = (float)excess;
                    lateness_next_ = (lateness_next_ + 1) % std::size(lateness_ring_);
                    lateness_count_ = std::min(lateness_count_ + 1, std::size(lateness_ring_));
                }
                item.ideal = item.arrival - std::chrono::microseconds((long long)(excess * 1000.0));
            }
            {
                // Frames and bytes after the oldest one in the window, over the
                // time they span: the rate actually received, not an average.
                rx_window.push_back({item.arrival, len});
                rx_window_bytes += len;
                while (item.arrival - rx_window.front().at > std::chrono::seconds(2)) {
                    rx_window_bytes -= rx_window.front().bytes;
                    rx_window.pop_front();
                }
                double span = std::chrono::duration<double>(item.arrival - rx_window.front().at).count();
                if (span >= 0.5) {
                    rx_fps_.store((float)((rx_window.size() - 1) / span));
                    rx_kbps_.store((float)((rx_window_bytes - rx_window.front().bytes) * 8.0 / span / 1000.0));
                }
                rx_last_ms_.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      item.arrival.time_since_epoch()).count());
            }
            if (!params.empty() && has_idr(annexb, len)) {
                item.data.reserve(params.size() + len);
                item.data.insert(item.data.end(), params.begin(), params.end());
            }
            item.data.insert(item.data.end(), annexb, annexb + len);
            recorder_.push_video(annexb, len, pts);   // recording uses arrival, not hold
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                queue.push_back(std::move(item));
            }
            queue_cv.notify_one();
            return !stop_.load();
        },
        stop_, error);

    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        stream_done = true;
    }
    queue_cv.notify_all();
    decode_thread.join();
    video_hold_ms_.store(0.0f);

    connected_.store(false);
    recorder_.stop("Local recording stopped because the stream ended.");
    broadcaster_.stop("Live broadcast stopped because the console stream ended.");
    audio_.stop();
    if (!stop_.load()) {
        // The console streams to one viewer: if the stream just closed and
        // someone is still watching, a browser or another PC took it.
        bool taken = error == kTakenOver;
        if (error.empty()) {
            StreamStatus status;
            std::string ignored_error;
            taken = client.fetch_status(status, ignored_error) && status.viewers > 0;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (taken)
            error_ = "Video: Glint is open in a browser or on another PC. Connect to watch here.";
        else
            error_ = error.empty() ? "Video stream stopped" : "Video: " + error;
        logf("video stream ended: %s", taken ? "another viewer took it"
                                       : error.empty() ? "closed by the console" : error.c_str());
    }
}

void App::status_worker() {
    PayloadClient client(url_);
    int recording_ticks = 0;
    while (!stop_.load()) {
        StreamStatus status;
        std::string error;
        if (client.fetch_status(status, error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = status;
        }
        // The payload emits IDRs on connect and on demand only (periodic ones
        // are latency spikes on Wi-Fi). While a PC recording is armed, keep
        // requesting one so the file starts promptly; while it records, ask
        // every 2 s so the file stays seekable.
        RecorderInfo::State rec = recorder_.info().state;
        recording_ticks = rec == RecorderInfo::State::Recording ? recording_ticks + 1 : 0;
        if (rec == RecorderInfo::State::Armed || (recording_ticks > 0 && recording_ticks % 2 == 0))
            client.post("/video/keyframe", nullptr, error);
        for (int i = 0; i < 10 && !stop_.load(); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool App::start_broadcast(const std::vector<BroadcastTarget>& targets, const BroadcastQuality& quality,
                          std::string& error) {
    if (!connected()) {
        error = "Connect to a console before starting a live broadcast.";
        return false;
    }
    StreamConfig cfg = config();
    bool with_audio = audio_.active();
    if (with_audio) audio_.add_sink(&broadcaster_);
    if (!broadcaster_.start(targets, quality, with_audio, cfg.audio_rate, error)) {
        audio_.remove_sink(&broadcaster_);
        return false;
    }
    return true;
}

void App::stop_broadcast(const std::string& reason) {
    broadcaster_.stop(reason);
    audio_.remove_sink(&broadcaster_);
}

bool App::start_local_recording(std::string& error) {
    if (!connected()) {
        error = "Choose a connected source before starting a PC recording.";
        return false;
    }
    StreamConfig cfg = config();
    std::string dir = media_dir();
    bool with_audio = audio_.active();
    if (with_audio) audio_.add_sink(&recorder_);
    if (!recorder_.start(dir, cfg, with_audio, error)) {
        audio_.remove_sink(&recorder_);
        return false;
    }
    rec_width_.store(cfg.width);
    rec_height_.store(cfg.height);
    // Ask for a keyframe so the file starts right away.
    std::string ignored;
    post("/video/keyframe", ignored);
    return true;
}

void App::stop_local_recording() {
    rec_width_.store(0);
    recorder_.stop();
    audio_.remove_sink(&recorder_);
}

std::string App::decoder_description() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return decoder_desc_;
}

bool App::take_screenshot(std::string& saved_path, std::string& error) {
    std::shared_ptr<Picture> frame;
    int width = 0, height = 0;
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frame = frame_;
        width = frame_width_;
        height = frame_height_;
        dir = media_dir_;
    }
    if (!frame || width <= 0 || height <= 0) {
        error = "Screenshot failed: no picture yet.";
        return false;
    }
    std::vector<uint8_t> rgba;
    if (!picture_to_rgba(*frame, rgba)) {
        error = "Screenshot could not be saved.";
        return false;
    }
    return save_screenshot(dir, rgba, width, height, saved_path, error);
}

std::string App::media_dir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return media_dir_;
}

void App::set_media_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    media_dir_ = dir;
}

std::vector<float> App::recent_lateness(size_t count) const {
    std::lock_guard<std::mutex> lock(lateness_mutex_);
    count = std::min(count, lateness_count_);
    std::vector<float> out(count);
    const size_t n = std::size(lateness_ring_);
    for (size_t i = 0; i < count; i++) out[i] = lateness_ring_[(lateness_next_ + n - count + i) % n];
    return out;
}

void App::received_rate(float& fps, float& kbps) const {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count();
    bool fresh = rx_last_ms_.load() != 0 && now - rx_last_ms_.load() < 2000;   // a stalled stream shows 0
    fps = fresh ? rx_fps_.load() : 0.0f;
    kbps = fresh ? rx_kbps_.load() : 0.0f;
}

double App::video_hold_target_ms() const {
    Latency mode = latency();
    if (mode == Latency::Lowest) return 0.0;   // show frames as they arrive
    // Balanced: just enough to cover 99 % of the recent jitter; a later frame
    // is shown late rather than dropped.
    if (mode == Latency::Balanced) return std::clamp(video_jitter_p99_ms_.load() + 4.0, 8.0, 120.0);
    // Smooth: wait out the network's jitter, and at least as long as the
    // sound takes to reach the speakers (when it is playing and synced).
    double hold = video_jitter_ms_.load() + 8.0;
    double latency = audio_.playout_latency_ms();
    if (video_sync_.load() && latency >= 0) hold = std::max(hold, latency + av_offset_ms_.load());
    return std::clamp(hold, 0.0, 800.0);
}

void App::set_latency(Latency latency) {
    latency_.store((int)latency);
    // Smooth also holds the picture to the sound, so it keeps a larger floor.
    audio_.set_auto_floor_ms(latency == Latency::Smooth ? 120 : latency == Latency::Balanced ? 60 : 40);
}

bool App::post(const std::string& path, std::string& error) {
    PayloadClient client(url_);
    return client.post(path, nullptr, error);
}

StreamConfig App::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

StreamStatus App::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

std::string App::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::string App::url() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return url_;
}

std::shared_ptr<const Picture> App::take_frame(uint64_t& last_seen) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_ || frame_seq_ == last_seen) return nullptr;
    last_seen = frame_seq_;
    return frame_;
}

} // namespace glint
