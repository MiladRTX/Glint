#include "audio.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace glint {
namespace {

constexpr int kCallbackSamples = 1024;
constexpr double kRingSeconds = 3.0;
constexpr int kMaxDelayMs = 400;
constexpr float kMaxVolume = 4.0f;
// Smallest usable prebuffer: one callback (1024) + one payload packet (1024)
// of arrival granularity + margin. A 0 ms delay setting plays at this floor.
constexpr int kMinPrimeFrames = 2 * kCallbackSamples + 256;
constexpr double kDeadbandFrames = 480.0;     // 10 ms @ 48 kHz
constexpr double kMaxRatioDeviation = 0.03;   // +-3 % drift/latency slew
constexpr double kWarmupSeconds = 5.0;
// Auto delay: cover the worst recent arrival lateness (how far packets ran
// behind the media clock, which is what drains the ring; a run of slightly
// late packets counts, not just one long gap) plus one callback and one packet
// of granularity. The memory halves every 60 s. Margin and floors were 50 and
// 100-150 ms; on Wi-Fi with gaps up to ~60 ms that sat ~190 ms behind the
// picture for no measured gain (no underruns at the lower values).
constexpr double kAutoMarginMs = 25.0;
constexpr double kAutoMinMsDefault = 60.0;
constexpr double kAutoHalfLifeS = 60.0;
constexpr double kAutoInitialGapMs = 70.0;

enum class Sample { F32, S16, S32 };

struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
    void lock() { while (flag.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { flag.clear(std::memory_order_release); }
};
struct SpinGuard {
    SpinLock& l;
    explicit SpinGuard(SpinLock& lock) : l(lock) { l.lock(); }
    ~SpinGuard() { l.unlock(); }
};

inline void relaxed_max(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

} // namespace

struct AudioPlayer::Impl {
    // Settings (persist across sessions) -------------------------------------
    std::atomic<bool> enabled{true};
    std::atomic<float> volume{1.0f};
    // 250 ms rides out the payload/Wi-Fi stalls seen on hardware in audio soaks (--audio-soak).
    std::atomic<int> delay_ms{250};
    std::atomic<bool> delay_auto{true};
    std::atomic<int> auto_floor_ms{(int)kAutoMinMsDefault};
    std::atomic<float> auto_target_ms{(float)(kAutoInitialGapMs + kAutoMarginMs)};
    std::atomic<bool> auto_boost{false};   // set by the callback on underrun
    // Network thread only:
    double gap_peak_ms = kAutoInitialGapMs;   // decayed peak of lateness excess
    bool jitter_init = false;
    std::chrono::steady_clock::time_point jitter_settle_until{};
    std::chrono::steady_clock::time_point jitter_t0{};
    uint64_t jitter_frames0 = 0;
    double jitter_baseline_ms = 0.0;
    std::atomic<double> level_pub{-1.0};    // smoothed ring level (frames), -1 unprimed

    // Lifecycle --------------------------------------------------------------
    std::mutex life;
    std::thread thread;
    std::atomic_bool stop_flag{false};
    SDL_AudioDeviceID device = 0;
    bool sdl_inited = false;
    std::string base_url;

    // Session format ---------------------------------------------------------
    Sample sample_type = Sample::F32;
    int in_channels = 2;
    int rate = 48000;
    std::string format_text;   // guarded by text_mutex
    std::string device_name;   // guarded by text_mutex
    std::atomic<int> rate_pub{0};
    std::atomic<bool> device_pub{false};
    std::atomic<bool> running{false};

    // Ring buffer (stereo float frames) --------------------------------------
    mutable SpinLock ring_lock;
    std::vector<float> ring;      // cap * 2
    uint64_t cap = 0;
    uint64_t write_pos = 0;       // absolute frame counters
    uint64_t read_pos = 0;
    // callback-owned state (touched under ring_lock or only by callback)
    bool primed = false;
    bool first_prime_seen = false;
    double frac = 0.0;
    double level_avg = 0.0;
    double ratio = 1.0;
    float gain_prev = 1.0f;
    double warm_frames = 0.0;
    std::vector<float> scratch;   // preallocated in start()

    // Stats ------------------------------------------------------------------
    std::atomic<uint64_t> packets{0}, frames_in{0}, frames_out{0}, callbacks{0};
    std::atomic<uint64_t> drop_events{0}, dropped_frames{0};
    std::atomic<uint64_t> underruns{0}, underrun_frames{0}, underruns_at_warm{0};
    std::atomic<uint64_t> nonfinite_in{0}, nonfinite_out{0}, clipped{0}, reconnects{0};
    std::atomic<bool> warmed{false};
    std::atomic<double> max_pkt_gap{0}, max_cb_gap{0};
    std::atomic<uint64_t> pkt_gaps50{0}, pkt_gaps100{0}, pkt_gaps200{0}, pkt_gaps400{0}, cb_gaps40{0};
    std::chrono::steady_clock::time_point last_pkt{}, last_cb{};   // single-writer each
    std::atomic<bool> streaming{false};
    std::atomic<bool> primed_pub{false};
    std::atomic<float> peak{0}, peak_max{0}, rms{0};
    std::atomic<double> ratio_pub{1.0};
    std::atomic<int> target_pub{0};

    std::mutex sink_mutex;
    std::vector<AudioSink*> sinks;

    mutable std::mutex text_mutex;
    std::string state = "stopped";
    std::string error;

    void set_state(const std::string& s) {
        std::lock_guard<std::mutex> g(text_mutex);
        state = s;
    }
    void set_error(const std::string& e) {
        std::lock_guard<std::mutex> g(text_mutex);
        error = e;
    }

    void reset_session_stats() {
        packets = 0; frames_in = 0; frames_out = 0; callbacks = 0;
        drop_events = 0; dropped_frames = 0;
        underruns = 0; underrun_frames = 0; underruns_at_warm = 0;
        nonfinite_in = 0; nonfinite_out = 0; clipped = 0; reconnects = 0;
        warmed = false; streaming = false; primed_pub = false;
        max_pkt_gap = 0; max_cb_gap = 0; pkt_gaps50 = 0; pkt_gaps100 = 0;
        pkt_gaps200 = 0; pkt_gaps400 = 0; cb_gaps40 = 0;
        last_pkt = {}; last_cb = {};
        peak = 0; peak_max = 0; rms = 0; ratio_pub = 1.0; target_pub = 0;
    }

    uint64_t target_frames() const {
        int ms = delay_auto.load(std::memory_order_relaxed)
                     ? (int)std::lround(auto_target_ms.load(std::memory_order_relaxed))
                     : delay_ms.load(std::memory_order_relaxed);
        ms = std::clamp(ms, 0, kMaxDelayMs);
        uint64_t t = (uint64_t)((double)rate * ms / 1000.0);
        return std::max<uint64_t>(t, kMinPrimeFrames);
    }

    // Producer: push stereo frames, dropping the oldest on overflow. ---------
    void push(const float* frames, uint64_t n) {
        if (n == 0) return;
        if (n > cap) {   // single packet larger than the ring: keep the tail
            dropped_frames += n - cap;
            drop_events++;
            frames += (n - cap) * 2;
            n = cap;
        }
        SpinGuard g(ring_lock);
        uint64_t used = write_pos - read_pos;
        if (used + n > cap) {
            uint64_t drop = used + n - cap;
            read_pos += drop;
            dropped_frames += drop;
            drop_events++;
        }
        uint64_t at = write_pos % cap;
        uint64_t first = std::min<uint64_t>(n, cap - at);
        std::memcpy(&ring[at * 2], frames, first * 2 * sizeof(float));
        if (first < n) std::memcpy(&ring[0], frames + first * 2, (n - first) * 2 * sizeof(float));
        write_pos += n;
    }

    // Converts a payload packet to interleaved stereo float. -----------------
    void ingest(const uint8_t* data, size_t len, std::vector<uint8_t>& carry,
                std::vector<float>& out) {
        size_t sample_bytes = sample_type == Sample::S16 ? 2 : 4;
        size_t frame_bytes = sample_bytes * (size_t)in_channels;

        const uint8_t* p = data;
        size_t n = len;
        std::vector<uint8_t> joined;
        if (!carry.empty()) {
            joined = carry;
            joined.insert(joined.end(), data, data + len);
            p = joined.data();
            n = joined.size();
            carry.clear();
        }
        size_t frames = n / frame_bytes;
        size_t used = frames * frame_bytes;
        if (used < n) carry.assign(p + used, p + n);

        out.resize(frames * 2);
        uint64_t bad = 0;
        for (size_t f = 0; f < frames; f++) {
            float ch[2] = {0.f, 0.f};
            for (int c = 0; c < in_channels && c < 2; c++) {
                const uint8_t* s = p + f * frame_bytes + (size_t)c * sample_bytes;
                float v;
                if (sample_type == Sample::F32) {
                    uint32_t bits = (uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                                    ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
                    std::memcpy(&v, &bits, 4);
                } else if (sample_type == Sample::S16) {
                    int16_t i = (int16_t)((uint16_t)s[0] | ((uint16_t)s[1] << 8));
                    v = (float)i / 32768.0f;
                } else {
                    int32_t i = (int32_t)((uint32_t)s[0] | ((uint32_t)s[1] << 8) |
                                          ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24));
                    v = (float)((double)i / 2147483648.0);
                }
                if (!std::isfinite(v)) { v = 0.f; bad++; }
                ch[c] = v;
            }
            if (in_channels == 1) ch[1] = ch[0];
            out[f * 2] = ch[0];
            out[f * 2 + 1] = ch[1];
        }
        if (bad) nonfinite_in += bad;
        frames_in += frames;
    }

    // Consumer: SDL callback. No allocation, short critical section. ---------
    void callback(float* out, int frames) {
        callbacks++;
        {
            auto now = std::chrono::steady_clock::now();
            if (last_cb.time_since_epoch().count() != 0) {
                double gap = std::chrono::duration<double, std::milli>(now - last_cb).count();
                if (gap > max_cb_gap.load(std::memory_order_relaxed))
                    max_cb_gap.store(gap, std::memory_order_relaxed);
                if (gap > 40.0) cb_gaps40++;
            }
            last_cb = now;
        }
        const bool en = enabled.load(std::memory_order_relaxed);
        const float vol = std::clamp(volume.load(std::memory_order_relaxed), 0.0f, kMaxVolume);
        const float gain_next = en ? vol : 0.0f;
        const uint64_t target = target_frames();
        target_pub.store((int)target, std::memory_order_relaxed);

        int produced = 0;           // output frames backed by real data
        double new_frac = frac;
        int copied = 0;
        int advance = 0;
        double use_ratio = ratio;
        bool rendering = false;

        {
            SpinGuard g(ring_lock);
            uint64_t avail = write_pos - read_pos;
            if (!primed && avail >= target) {
                // Start at the target: a backlog burst on (re)connect would
                // otherwise be played out 3 % fast for seconds.
                if (avail > target + (uint64_t)kCallbackSamples) {
                    read_pos += avail - target;
                    avail = target;
                }
                primed = true;
                first_prime_seen = true;
                frac = 0.0;
                level_avg = (double)avail;
                ratio = 1.0;
            }
            rendering = primed;
            if (primed) {
                level_avg += ((double)avail - level_avg) * 0.02;
                double err = level_avg - (double)target;
                double want = 1.0;
                if (std::fabs(err) > kDeadbandFrames) {
                    double dev = std::min(kMaxRatioDeviation,
                                          (std::fabs(err) - kDeadbandFrames) / (rate * 2.0));
                    want = err > 0 ? 1.0 + dev : 1.0 - dev;
                }
                ratio += (want - ratio) * 0.1;
                if (std::fabs(ratio - 1.0) < 1e-5 && want == 1.0) ratio = 1.0;
                use_ratio = ratio;

                // How many frames can we render from what is buffered?
                // Output k reads source index floor(frac + k*ratio) and +1.
                int max_copy = (int)std::min<uint64_t>(
                    avail, (uint64_t)((double)frames * (1.0 + kMaxRatioDeviation)) + 4);
                max_copy = std::min<int>(max_copy, (int)(scratch.size() / 2));
                copied = max_copy;
                uint64_t at = read_pos % cap;
                uint64_t first = std::min<uint64_t>((uint64_t)copied, cap - at);
                std::memcpy(scratch.data(), &ring[at * 2], first * 2 * sizeof(float));
                if (first < (uint64_t)copied)
                    std::memcpy(scratch.data() + first * 2, &ring[0],
                                ((uint64_t)copied - first) * 2 * sizeof(float));

                for (int k = 0; k < frames; k++) {
                    int idx = (int)(frac + (double)k * use_ratio);
                    if (idx + 1 >= copied) break;
                    produced++;
                }
                double end = frac + (double)produced * use_ratio;
                advance = (int)end;
                new_frac = end - (double)advance;
                read_pos += (uint64_t)advance;
                if (produced < frames) {
                    // Ran dry. Re-prime to the delay target instead of
                    // stuttering on whatever trickles in.
                    primed = false;
                }
            }
        }

        // Rendering happens outside the lock, from the private scratch copy.
        const double start_frac = frac;
        const float g0 = gain_prev;
        float peak_cb = 0.f;
        double sq = 0.0;
        uint64_t clip = 0, bad = 0;

        for (int k = 0; k < frames; k++) {
            float l = 0.f, r = 0.f;
            if (k < produced) {
                double pos = start_frac + (double)k * use_ratio;
                int i0 = (int)pos;
                float f = (float)(pos - (double)i0);
                const float* a = &scratch[(size_t)i0 * 2];
                const float* b = a + 2;
                if (f == 0.0f) { l = a[0]; r = a[1]; }
                else {
                    l = a[0] + (b[0] - a[0]) * f;
                    r = a[1] + (b[1] - a[1]) * f;
                }
                float g = g0 + (gain_next - g0) * ((float)(k + 1) / (float)frames);
                l *= g; r *= g;
            }
            if (!std::isfinite(l)) { l = 0.f; bad++; }
            if (!std::isfinite(r)) { r = 0.f; bad++; }
            if (l > 1.f) { l = 1.f; clip++; } else if (l < -1.f) { l = -1.f; clip++; }
            if (r > 1.f) { r = 1.f; clip++; } else if (r < -1.f) { r = -1.f; clip++; }
            out[k * 2] = l;
            out[k * 2 + 1] = r;
            peak_cb = std::max(peak_cb, std::max(std::fabs(l), std::fabs(r)));
            sq += (double)l * l + (double)r * r;
        }

        gain_prev = gain_next;
        frac = new_frac;

        frames_out += (uint64_t)produced;
        if (clip) clipped += clip;
        if (bad) nonfinite_out += bad;
        float prev_peak = peak.load(std::memory_order_relaxed);
        peak.store(std::max(peak_cb, prev_peak * 0.9f), std::memory_order_relaxed);
        relaxed_max(peak_max, peak_cb);
        rms.store((float)std::sqrt(sq / (double)(frames * 2)), std::memory_order_relaxed);
        ratio_pub.store(use_ratio, std::memory_order_relaxed);
        primed_pub.store(primed, std::memory_order_relaxed);
        level_pub.store(primed ? level_avg : -1.0, std::memory_order_relaxed);

        if (rendering && produced < frames) {
            underruns++;
            underrun_frames += (uint64_t)(frames - produced);
            auto_boost.store(true, std::memory_order_relaxed);
        }

        if (first_prime_seen) {
            warm_frames += frames;
            if (!warmed.load(std::memory_order_relaxed) && warm_frames >= kWarmupSeconds * rate) {
                underruns_at_warm.store(underruns.load());
                warmed.store(true);
            }
        }
    }
};

namespace {

void SDLCALL sdl_callback(void* userdata, Uint8* stream, int len) {
    auto* impl = static_cast<AudioPlayer::Impl*>(userdata);
    impl->callback(reinterpret_cast<float*>(stream), len / (int)(2 * sizeof(float)));
}

} // namespace

int process_thread_count() {
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    DWORD pid = GetCurrentProcessId();
    int count = 0;
    if (Thread32First(snap, &entry)) {
        do {
            if (entry.th32OwnerProcessID == pid) count++;
        } while (Thread32Next(snap, &entry));
    }
    CloseHandle(snap);
    return count;
#else
    return -1;
#endif
}

AudioPlayer::AudioPlayer() : impl_(new Impl) {}

AudioPlayer::~AudioPlayer() {
    stop();
}

void AudioPlayer::set_enabled(bool on) { impl_->enabled.store(on); }
bool AudioPlayer::enabled() const { return impl_->enabled.load(); }
void AudioPlayer::set_volume(float v) {
    if (!std::isfinite(v)) v = 1.0f;
    impl_->volume.store(std::clamp(v, 0.0f, kMaxVolume));
}
float AudioPlayer::volume() const { return impl_->volume.load(); }
void AudioPlayer::set_delay_ms(int ms) { impl_->delay_ms.store(std::clamp(ms, 0, kMaxDelayMs)); }
int AudioPlayer::delay_ms() const { return impl_->delay_ms.load(); }
void AudioPlayer::set_delay_auto(bool on) { impl_->delay_auto.store(on); }
void AudioPlayer::set_auto_floor_ms(int ms) { impl_->auto_floor_ms.store(std::clamp(ms, 30, kMaxDelayMs)); }
bool AudioPlayer::delay_auto() const { return impl_->delay_auto.load(); }

int AudioPlayer::effective_delay_ms() const {
    const Impl& s = *impl_;
    int ms = s.delay_auto.load() ? (int)std::lround(s.auto_target_ms.load()) : s.delay_ms.load();
    return std::clamp(ms, 0, kMaxDelayMs);
}

double AudioPlayer::playout_latency_ms() const {
    const Impl& s = *impl_;
    if (!s.running.load() || !s.enabled.load()) return -1.0;
    double level = s.level_pub.load(std::memory_order_relaxed);
    int rate = s.rate_pub.load();
    if (level < 0 || rate <= 0) return -1.0;
    // Ring wait + one SDL buffer + an allowance for the OS mixer.
    return 1000.0 * (level + kCallbackSamples) / rate + 10.0;
}

bool AudioPlayer::active() const {
    return impl_->running.load();
}

std::string AudioPlayer::last_error() const {
    std::lock_guard<std::mutex> g(impl_->text_mutex);
    return impl_->error;
}

void AudioPlayer::add_sink(AudioSink* sink) {
    std::lock_guard<std::mutex> g(impl_->sink_mutex);
    auto& v = impl_->sinks;
    if (sink && std::find(v.begin(), v.end(), sink) == v.end()) v.push_back(sink);
}

void AudioPlayer::remove_sink(AudioSink* sink) {
    std::lock_guard<std::mutex> g(impl_->sink_mutex);
    auto& v = impl_->sinks;
    v.erase(std::remove(v.begin(), v.end(), sink), v.end());
}

bool AudioPlayer::start(const std::string& base_url, const StreamConfig& config) {
    stop();
    Impl& s = *impl_;
    std::lock_guard<std::mutex> life(s.life);

    s.reset_session_stats();
    s.set_error("");

    if (!config.has_audio) {
        s.set_state("no audio channel");
        return false;
    }

    const std::string& codec = config.audio_codec;
    if (codec == "pcm_s16le" || (codec.empty() && config.audio_bits == 16)) s.sample_type = Sample::S16;
    else if (codec == "pcm_s32le") s.sample_type = Sample::S32;
    else if (codec == "pcm_f32le" || codec.empty()) s.sample_type = Sample::F32;
    else {
        s.set_state("unsupported audio codec");
        s.set_error("unsupported audio codec: " + codec);
        return false;
    }
    s.in_channels = std::clamp(config.audio_channels > 0 ? config.audio_channels : 2, 1, 8);
    s.rate = config.audio_rate > 0 ? config.audio_rate : 48000;
    s.base_url = base_url;
    s.rate_pub.store(s.rate);
    {
        char text[96];
        std::snprintf(text, sizeof(text), "%s %dch %d Hz",
                      codec.empty() ? "pcm_f32le" : codec.c_str(), s.in_channels, s.rate);
        std::lock_guard<std::mutex> g(s.text_mutex);
        s.format_text = text;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        s.set_state("audio unavailable");
        s.set_error(std::string("SDL audio init: ") + SDL_GetError());
        return false;
    }
    s.sdl_inited = true;

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = s.rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = kCallbackSamples;
    want.callback = sdl_callback;
    want.userdata = &s;
    s.device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);  // SDL converts if needed
    if (s.device == 0) {
        s.set_state("audio device failed");
        s.set_error(std::string("SDL_OpenAudioDevice: ") + SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        s.sdl_inited = false;
        return false;
    }
    const char* name = SDL_GetAudioDeviceName(0, 0);
    {
        std::lock_guard<std::mutex> g(s.text_mutex);
        s.device_name = name ? name : "default";
    }

    // Reset ring/callback state before the device starts pulling.
    s.cap = (uint64_t)(kRingSeconds * s.rate);
    s.ring.assign((size_t)s.cap * 2, 0.0f);
    s.scratch.assign((size_t)(have.samples * 2 + 64) * 2, 0.0f);
    s.write_pos = s.read_pos = 0;
    s.primed = false;
    s.first_prime_seen = false;
    s.frac = 0.0;
    s.level_avg = 0.0;
    s.level_pub.store(-1.0);
    s.ratio = 1.0;
    s.gap_peak_ms = kAutoInitialGapMs;
    s.jitter_init = false;
    s.auto_target_ms.store((float)(kAutoInitialGapMs + kAutoMarginMs));
    s.auto_boost.store(false);
    s.gain_prev = s.enabled.load() ? s.volume.load() : 0.0f;
    s.warm_frames = 0.0;

    s.stop_flag.store(false);
    s.set_state("buffering");
    s.thread = std::thread([&s]() {
#ifdef _WIN32
        // The software H.264 decode saturates the CPU; keep payload audio ahead of it.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
        std::vector<uint8_t> carry;
        std::vector<float> converted;
        PayloadClient client(s.base_url);
        while (!s.stop_flag.load()) {
            std::string err;
            s.streaming = true;
            s.jitter_init = false;   // new connection: new arrival/media baseline
            client.stream_audio(
                [&](const uint8_t* pcm, size_t len, uint32_t) {
                    if (s.stop_flag.load()) return false;
                    s.packets++;
                    {
                        auto now = std::chrono::steady_clock::now();
                        if (s.last_pkt.time_since_epoch().count() != 0) {
                            double gap = std::chrono::duration<double, std::milli>(
                                now - s.last_pkt).count();
                            if (gap > s.max_pkt_gap.load(std::memory_order_relaxed))
                                s.max_pkt_gap.store(gap, std::memory_order_relaxed);
                            if (gap > 50.0) s.pkt_gaps50++;
                            if (gap > 100.0) s.pkt_gaps100++;
                            if (gap > 200.0) s.pkt_gaps200++;
                            if (gap > 400.0) s.pkt_gaps400++;
                        }
                        // Lateness of this packet against the media clock. Not
                        // measured for 2 s after (re)connecting: the payload
                        // flushes its audio backlog in a burst, which would
                        // read as huge lateness and pin the buffer at maximum.
                        if (s.jitter_init && now >= s.jitter_settle_until) {
                            double gap = std::chrono::duration<double, std::milli>(now - s.last_pkt).count();
                            double media_ms = 1000.0 * (double)(s.frames_in.load() - s.jitter_frames0) / s.rate;
                            double arrival_ms = std::chrono::duration<double, std::milli>(
                                now - s.jitter_t0).count();
                            double lateness = arrival_ms - media_ms;
                            // Baseline = earliest arrival seen, allowed to creep
                            // up 2 ms/s so it follows clock drift.
                            s.jitter_baseline_ms = std::min(lateness, s.jitter_baseline_ms + 2.0 * gap / 1000.0);
                            double excess = lateness - s.jitter_baseline_ms;
                            s.gap_peak_ms = std::max(
                                excess, s.gap_peak_ms * std::pow(0.5, gap / 1000.0 / kAutoHalfLifeS));
                        }
                        if (!s.jitter_init || now < s.jitter_settle_until) {
                            if (!s.jitter_init) s.jitter_settle_until = now + std::chrono::seconds(2);
                            s.jitter_init = true;
                            s.jitter_t0 = now;
                            s.jitter_frames0 = s.frames_in.load();
                            s.jitter_baseline_ms = 0.0;
                        }
                        if (s.auto_boost.exchange(false)) {
                            // We ran dry anyway: buffer deeper right away.
                            s.gap_peak_ms = std::max(
                                s.gap_peak_ms, s.auto_target_ms.load() * 1.5 - kAutoMarginMs);
                        }
                        s.auto_target_ms.store((float)std::clamp(
                            s.gap_peak_ms + kAutoMarginMs, (double)s.auto_floor_ms.load(), (double)kMaxDelayMs));
                        s.last_pkt = now;
                    }
                    s.ingest(pcm, len, carry, converted);
                    s.push(converted.data(), converted.size() / 2);
                    {
                        std::lock_guard<std::mutex> g(s.sink_mutex);
                        for (AudioSink* sink : s.sinks)
                            sink->on_audio(converted.data(), converted.size() / 2, s.rate);
                    }
                    return !s.stop_flag.load();
                },
                s.stop_flag, err, 3000);
            s.streaming = false;
            carry.clear();
            if (s.stop_flag.load()) break;
            s.reconnects++;
            s.set_error(err.empty() ? "audio stream closed" : "audio: " + err);
            for (int i = 0; i < 10 && !s.stop_flag.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        s.streaming = false;
    });

    s.device_pub.store(true);
    s.running.store(true);
    SDL_PauseAudioDevice(s.device, 0);
    return true;
}

void AudioPlayer::stop() {
    Impl& s = *impl_;
    std::lock_guard<std::mutex> life(s.life);
    if (!s.thread.joinable() && s.device == 0 && !s.sdl_inited) return;

    s.running.store(false);
    s.stop_flag.store(true);
    if (s.thread.joinable()) s.thread.join();
    if (s.device) {
        SDL_PauseAudioDevice(s.device, 1);
        SDL_CloseAudioDevice(s.device);   // returns after the callback has finished
        s.device = 0;
    }
    s.device_pub.store(false);
    if (s.sdl_inited) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        s.sdl_inited = false;
    }
    {
        SpinGuard g(s.ring_lock);          // drain
        s.write_pos = s.read_pos = 0;
        s.primed = false;
    }
    s.streaming = false;
    s.primed_pub = false;
    s.set_state("stopped");
}

AudioStats AudioPlayer::stats() const {
    const Impl& s = *impl_;
    AudioStats st;
    st.device_open = s.device_pub.load();
    st.streaming = s.streaming.load();
    st.primed = s.primed_pub.load();
    st.rate = s.rate_pub.load();
    {
        std::lock_guard<std::mutex> g(s.text_mutex);
        st.state = s.state;
        st.format = s.format_text;
        st.device = s.device_name;
    }
    st.packets = s.packets; st.frames_in = s.frames_in; st.frames_out = s.frames_out;
    st.callbacks = s.callbacks;
    st.drop_events = s.drop_events; st.dropped_frames = s.dropped_frames;
    st.underruns = s.underruns; st.underrun_frames = s.underrun_frames;
    st.warmed_up = s.warmed.load();
    st.underruns_after_warmup = st.warmed_up ? st.underruns - s.underruns_at_warm.load() : 0;
    st.nonfinite_in = s.nonfinite_in; st.nonfinite_out = s.nonfinite_out;
    st.clipped_samples = s.clipped; st.reconnects = s.reconnects;
    st.ratio = s.ratio_pub.load();
    st.peak = s.peak.load(); st.peak_max = s.peak_max.load(); st.rms = s.rms.load();
    st.max_packet_gap_ms = s.max_pkt_gap.load();
    st.packet_gaps_50ms = s.pkt_gaps50; st.packet_gaps_100ms = s.pkt_gaps100;
    st.packet_gaps_200ms = s.pkt_gaps200; st.packet_gaps_400ms = s.pkt_gaps400;
    st.max_callback_gap_ms = s.max_cb_gap.load();
    st.auto_delay = s.delay_auto.load();
    st.playout_ms = playout_latency_ms();
    st.callback_gaps_40ms = s.cb_gaps40;
    int rate = st.rate;
    st.target_ms = rate ? 1000.0 * s.target_pub.load() / rate : 0.0;
    uint64_t used = 0;
    if (st.device_open) {
        SpinGuard g(s.ring_lock);
        used = s.write_pos - s.read_pos;
    }
    st.buffered_ms = rate ? 1000.0 * (double)used / rate : 0.0;
    if (st.state == "buffering" && st.primed) st.state = "playing";
    return st;
}

} // namespace glint
