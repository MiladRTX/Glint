#include "audio.h"
#include "log.h"
#include "winutil.h"

#include <tlhelp32.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace glint {

// --- SDL audio subsystem ------------------------------------------------------------

namespace {
std::mutex g_sdl_mutex;
int g_sdl_refs = 0;
}

bool sdl_audio_acquire(std::string& error)
{
    std::lock_guard<std::mutex> lock(g_sdl_mutex);
    if (g_sdl_refs == 0) {
        SDL_SetMainReady();
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            error = SDL_GetError();
            return false;
        }
    }
    g_sdl_refs++;
    return true;
}

void sdl_audio_release()
{
    std::lock_guard<std::mutex> lock(g_sdl_mutex);
    if (g_sdl_refs <= 0) return;
    if (--g_sdl_refs == 0) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int process_thread_count()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    int count = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) count++;
            te.dwSize = sizeof(te);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return count;
}

// --- player ---------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

double ms_between(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

enum class Sample { F32, S16, S32 };

constexpr double kRingSeconds = 3.0;
constexpr double kMixerAllowanceMs = 10.0;
constexpr double kMarginMs = 15.0;   // an underrun raises the target by half at once
constexpr double kStartTargetMs = 95.0;
constexpr double kIgnoreAfterConnectS = 2.0;
constexpr double kOutageMs = 250.0;   // later than this is a stall, not network jitter
constexpr double kWarmupS = 5.0;

}  // namespace

struct AudioPlayer::Impl {
    // Settings, readable from any thread.
    std::atomic<bool> enabled{true};
    std::atomic<float> volume{1.0f};
    std::atomic<int> delay_ms{250};
    std::atomic<bool> delay_auto{true};
    std::atomic<int> floor_ms{40};
    std::atomic<double> auto_target_ms{kStartTargetMs};
    std::atomic<bool> underrun_bump{false};

    // Session.
    std::mutex session_mutex;
    std::atomic<bool> active{false};
    std::atomic<bool> streaming{false};
    std::atomic<bool> net_stop{false};
    std::thread net;
    SDL_AudioDeviceID dev = 0;
    std::string url;
    int rate = 48000;
    int channels = 2;
    Sample sample = Sample::F32;
    int device_frames = 512;

    // Ring and playback state, under ring_mutex (the callback takes it briefly).
    mutable std::mutex ring_mutex;
    std::vector<float> ring;   // stereo frames
    size_t cap = 0, rd = 0, count = 0;
    bool primed = false;
    double pos = 0.0;          // fractional read position for the resampler
    double ratio = 1.0;
    double level_ms = 0.0;     // smoothed buffer level
    float gain = 0.0f;         // gain applied at the end of the last callback
    Clock::time_point last_callback{};
    Clock::time_point first_prime{};
    bool ever_primed = false;
    AudioStats st;             // counters (strings live under info_mutex)
    std::atomic<double> playout{-1.0};
    std::vector<float> scratch;   // callback input copy, sized at session start

    mutable std::mutex info_mutex;
    std::string state = "Stopped";
    std::string device;
    std::string format;
    std::string last_error;

    std::mutex sink_mutex;
    Sink sink;

    double min_workable_ms() const { return 2.0 * device_frames * 1000.0 / rate + 5.0; }

    double target_ms() const
    {
        double t = delay_auto.load() ? auto_target_ms.load() : (double)delay_ms.load();
        return std::clamp(std::max(t, min_workable_ms()), 0.0, 400.0);
    }

    void set_info(const char* new_state, const std::string& error)
    {
        std::lock_guard<std::mutex> lock(info_mutex);
        state = new_state;
        if (!error.empty()) last_error = error;
    }

    // Drops `n` of the oldest frames (ring_mutex held).
    void drop_oldest(size_t n)
    {
        n = std::min(n, count);
        rd = (rd + n) % cap;
        count -= n;
    }

    void push(const float* stereo, size_t frames)
    {
        std::lock_guard<std::mutex> lock(ring_mutex);
        if (frames > cap) {
            stereo += (frames - cap) * 2;
            frames = cap;
        }
        if (count + frames > cap) {
            size_t over = count + frames - cap;
            drop_oldest(over);
            st.drop_events++;
            st.dropped_frames += over;
        }
        size_t wr = (rd + count) % cap;
        for (size_t i = 0; i < frames; i++) {
            ring[wr * 2] = stereo[i * 2];
            ring[wr * 2 + 1] = stereo[i * 2 + 1];
            if (++wr == cap) wr = 0;
        }
        count += frames;
    }

    static void SDLCALL callback(void* user, Uint8* stream, int len)
    {
        ((Impl*)user)->fill((float*)stream, (size_t)len / (2 * sizeof(float)));
    }

    void fill(float* out, size_t n)
    {
        Clock::time_point now = Clock::now();
        const double target = target_ms();
        const size_t target_frames = (size_t)(target * rate / 1000.0);
        size_t got = 0;            // input frames copied to scratch
        double start_pos = 0.0, step = 1.0;
        bool play = false;
        {
            std::lock_guard<std::mutex> lock(ring_mutex);
            st.callbacks++;
            if (last_callback.time_since_epoch().count() != 0) {
                double gap = ms_between(last_callback, now);
                st.max_callback_gap_ms = std::max(st.max_callback_gap_ms, gap);
                if (gap > 40.0) st.callback_gaps_40ms++;
            }
            last_callback = now;
            st.target_ms = target;

            if (!primed && count >= target_frames && count > 0) {
                if (count > target_frames + (size_t)device_frames) drop_oldest(count - target_frames);
                primed = true;
                pos = 0.0;
                level_ms = count * 1000.0 / rate;
                if (!ever_primed) {
                    ever_primed = true;
                    first_prime = now;
                }
            }
            if (primed && count > target_frames + std::max<size_t>(2 * (size_t)device_frames, (size_t)rate / 20)) {
                // A backlog after a stall (the console sends what it held back in one burst):
                // drift correction at 3 % would take tens of seconds to catch up, so skip it.
                st.drop_events++;
                st.dropped_frames += count - target_frames;
                drop_oldest(count - target_frames);
                level_ms = count * 1000.0 / rate;
            }
            if (primed) {
                // Drift correction: nudge the playback rate so the level sits on the target.
                double level = count * 1000.0 / rate;
                level_ms += (level - level_ms) * 0.05;
                double err = level_ms - target;
                double want = 1.0;
                if (std::fabs(err) > 10.0) want = 1.0 + std::clamp((err - std::copysign(10.0, err)) * 0.0004, -0.03, 0.03);
                ratio += std::clamp(want - ratio, -0.0005, 0.0005);
                step = ratio;
                start_pos = pos;
                size_t need = (size_t)std::floor(pos + (double)(n - 1) * step) + 2;
                got = std::min(need, count);
                size_t idx = rd;
                for (size_t i = 0; i < got; i++) {
                    scratch[i * 2] = ring[idx * 2];
                    scratch[i * 2 + 1] = ring[idx * 2 + 1];
                    if (++idx == cap) idx = 0;
                }
                if (got < need) {
                    // Ran dry: play what there is, then wait for the target again.
                    drop_oldest(got);
                    primed = false;
                    st.underruns++;
                    if (ever_primed && ms_between(first_prime, now) > kWarmupS * 1000.0) st.underruns_after_warmup++;
                    underrun_bump.store(true);
                    if (delay_auto.load())
                        auto_target_ms.store(std::min(400.0, std::max(auto_target_ms.load(), target) * 1.5));
                } else {
                    size_t used = (size_t)std::floor(pos + (double)n * step);
                    drop_oldest(std::min(used, count));
                    pos = pos + (double)n * step - (double)used;
                }
                play = true;
            }
            st.buffered_ms = count * 1000.0 / rate;
            st.ratio = primed ? ratio : 1.0;
        }

        // Resample (linear) and apply the gain ramp outside the lock.
        float g1 = enabled.load() ? std::clamp(volume.load(), 0.0f, 4.0f) : 0.0f;
        float g0 = gain;
        size_t produced = 0;
        uint64_t clipped = 0, nonfinite = 0;
        float peak = 0.0f;
        double sum_sq = 0.0;
        for (size_t i = 0; i < n; i++) {
            float l = 0.0f, r = 0.0f;
            if (play) {
                double p = start_pos + (double)i * step;
                size_t a = (size_t)p;
                if (a + 1 < got) {
                    float f = (float)(p - (double)a);
                    l = scratch[a * 2] + (scratch[a * 2 + 2] - scratch[a * 2]) * f;
                    r = scratch[a * 2 + 1] + (scratch[a * 2 + 3] - scratch[a * 2 + 1]) * f;
                    produced++;
                }
            }
            float g = g0 + (g1 - g0) * (float)(i + 1) / (float)n;
            float s[2] = {l * g, r * g};
            for (float& x : s) {
                if (!std::isfinite(x)) {
                    x = 0.0f;
                    nonfinite++;
                }
                if (x > 1.0f || x < -1.0f) {
                    x = x > 0 ? 1.0f : -1.0f;
                    clipped++;
                }
                peak = std::max(peak, std::fabs(x));
                sum_sq += (double)x * x;
            }
            out[i * 2] = s[0];
            out[i * 2 + 1] = s[1];
        }
        gain = g1;

        std::lock_guard<std::mutex> lock(ring_mutex);
        st.frames_out += produced;
        if (play && produced < n && !primed) st.underrun_frames += n - produced;
        st.clipped_samples += clipped;
        st.nonfinite_out += nonfinite;
        st.peak = std::max(peak, st.peak * 0.85f);
        st.peak_max = std::max(st.peak_max, peak);
        st.rms = (float)std::sqrt(sum_sq / (double)(n * 2));
        if (ever_primed) st.warmed_up = ms_between(first_prime, now) > kWarmupS * 1000.0;
        st.primed = primed;
        bool on = primed && enabled.load();
        playout.store(on ? level_ms + device_frames * 1000.0 / rate + kMixerAllowanceMs : -1.0);
    }

    void network_loop()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const size_t in_frame = (size_t)channels * (sample == Sample::S16 ? 2 : 4);
        std::vector<uint8_t> carry;
        std::vector<float> stereo;
        bool first_connect = true;

        while (!net_stop.load()) {
            if (!first_connect) {
                std::lock_guard<std::mutex> lock(ring_mutex);
                st.reconnects++;
            }
            first_connect = false;
            carry.clear();
            Clock::time_point connected_at = Clock::now();
            Clock::time_point last_packet{};
            bool measuring = false;
            double base = 0.0, last_t = 0.0;
            double peak_late = 0.0;
            double ignore_until = 0.0;
            uint64_t media_frames = 0;

            auto on_packet = [&](const uint8_t* data, size_t len, uint32_t) {
                Clock::time_point now = Clock::now();
                streaming.store(true);
                // Raw bytes to stereo float; a frame split across packets waits for the rest.
                carry.insert(carry.end(), data, data + len);
                size_t frames = carry.size() / in_frame;
                stereo.resize(frames * 2);
                uint64_t bad = 0;
                for (size_t i = 0; i < frames; i++) {
                    const uint8_t* f = carry.data() + i * in_frame;
                    float ch[2];
                    for (int c = 0; c < 2; c++) {
                        int src = std::min(c, channels - 1);
                        float v;
                        if (sample == Sample::F32) {
                            std::memcpy(&v, f + src * 4, 4);
                            if (!std::isfinite(v)) {
                                v = 0.0f;
                                if (c == src) bad++;
                            }
                        } else if (sample == Sample::S16) {
                            int16_t x;
                            std::memcpy(&x, f + src * 2, 2);
                            v = (float)x / 32768.0f;
                        } else {
                            int32_t x;
                            std::memcpy(&x, f + src * 4, 4);
                            v = (float)((double)x / 2147483648.0);
                        }
                        ch[c] = v;
                    }
                    stereo[i * 2] = ch[0];
                    stereo[i * 2 + 1] = ch[1];
                }
                carry.erase(carry.begin(), carry.begin() + (ptrdiff_t)(frames * in_frame));

                {
                    std::lock_guard<std::mutex> lock(ring_mutex);
                    st.packets++;
                    st.frames_in += frames;
                    st.nonfinite_in += bad;
                    if (last_packet.time_since_epoch().count() != 0) {
                        double gap = ms_between(last_packet, now);
                        st.max_packet_gap_ms = std::max(st.max_packet_gap_ms, gap);
                        if (gap > 50) st.packet_gaps_50ms++;
                        if (gap > 100) st.packet_gaps_100ms++;
                        if (gap > 200) st.packet_gaps_200ms++;
                        if (gap > 400) st.packet_gaps_400ms++;
                    }
                }
                last_packet = now;
                if (frames) push(stereo.data(), frames);
                {
                    std::lock_guard<std::mutex> lock(sink_mutex);
                    if (sink && frames) sink(stereo.data(), frames, rate, now);
                }

                // Automatic target: how late packets arrive against the media clock.
                double t = std::chrono::duration<double>(now - connected_at).count();
                double offset = t - (double)media_frames / rate;
                media_frames += frames;
                if (measuring && (offset - base) * 1000.0 > kOutageMs) {
                    // An outage, not jitter: no buffer would have covered it, so it must not
                    // hold the target up for a minute. Measure afresh once its backlog is in.
                    measuring = false;
                    ignore_until = t + kIgnoreAfterConnectS;
                }
                if (t >= kIgnoreAfterConnectS && t >= ignore_until) {
                    if (!measuring) {
                        measuring = true;
                        base = offset;
                        last_t = t;
                    }
                    double dt = t - last_t;
                    last_t = t;
                    base = std::min(base + 0.002 * dt, offset);   // follows clock drift slowly
                    double late_ms = (offset - base) * 1000.0;
                    peak_late = std::max(late_ms, peak_late * std::exp(-dt / 20.0));
                    if (underrun_bump.exchange(false))
                        peak_late = std::max(peak_late, auto_target_ms.load() - kMarginMs);
                    double floor = (double)floor_ms.load();
                    auto_target_ms.store(std::clamp(peak_late + kMarginMs, floor, 400.0));
                }
                return !net_stop.load();
            };

            PayloadClient client(url);
            std::string error;
            bool ok = client.stream_audio(on_packet, net_stop, error, 3000);
            streaming.store(false);
            if (net_stop.load()) break;
            std::string why = ok ? "The audio stream ended." : "Audio stream: " + error;
            set_info("Reconnecting", why);
            logf("audio: %s; reconnecting", why.c_str());
            for (int i = 0; i < 10 && !net_stop.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
};

AudioPlayer::AudioPlayer() : impl_(new Impl) {}

AudioPlayer::~AudioPlayer()
{
    stop_session();
}

void AudioPlayer::set_enabled(bool on)
{
    impl_->enabled.store(on);
}

bool AudioPlayer::enabled() const
{
    return impl_->enabled.load();
}

void AudioPlayer::set_volume(float v)
{
    if (!std::isfinite(v)) v = 1.0f;
    impl_->volume.store(std::clamp(v, 0.0f, 4.0f));
}

float AudioPlayer::volume() const
{
    return impl_->volume.load();
}

void AudioPlayer::set_delay_ms(int ms)
{
    impl_->delay_ms.store(std::clamp(ms, 0, 400));
}

int AudioPlayer::delay_ms() const
{
    return impl_->delay_ms.load();
}

void AudioPlayer::set_delay_auto(bool on)
{
    impl_->delay_auto.store(on);
}

bool AudioPlayer::delay_auto() const
{
    return impl_->delay_auto.load();
}

int AudioPlayer::effective_delay_ms() const
{
    return (int)std::lround(impl_->target_ms());
}

bool AudioPlayer::active() const
{
    return impl_->active.load();
}

std::string AudioPlayer::last_error() const
{
    std::lock_guard<std::mutex> lock(impl_->info_mutex);
    return impl_->last_error;
}

AudioStats AudioPlayer::stats() const
{
    Impl& d = *impl_;
    AudioStats s;
    {
        std::lock_guard<std::mutex> lock(d.ring_mutex);
        s = d.st;
        s.primed = d.active.load() && d.primed;
        s.buffered_ms = d.active.load() ? d.count * 1000.0 / d.rate : 0.0;
    }
    s.device_open = d.active.load();
    s.streaming = d.streaming.load();
    s.auto_delay = d.delay_auto.load();
    s.playout_ms = d.active.load() ? d.playout.load() : -1.0;
    {
        std::lock_guard<std::mutex> lock(d.info_mutex);
        s.state = d.state;
        s.device = d.device;
        s.format = d.format;
    }
    if (d.active.load()) s.state = !s.streaming ? "Connecting" : s.primed ? "Playing" : "Buffering";
    return s;
}

bool AudioPlayer::start_session(const std::string& url, const StreamConfig& config)
{
    stop_session();
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.session_mutex);
    {
        std::lock_guard<std::mutex> rl(d.ring_mutex);
        d.st = AudioStats();
        d.st.rate = config.audio_rate;
        d.primed = false;
        d.ever_primed = false;
        d.ratio = 1.0;
        d.gain = 0.0f;
        d.last_callback = Clock::time_point{};
    }
    d.auto_target_ms.store(std::max(kStartTargetMs, (double)d.floor_ms.load()));
    d.underrun_bump.store(false);
    d.playout.store(-1.0);
    {
        std::lock_guard<std::mutex> il(d.info_mutex);
        d.last_error.clear();
        d.device.clear();
        d.format.clear();
    }

    if (!config.has_audio) {
        d.set_info("No audio channel", "The console is not sending sound.");
        return false;
    }
    std::string codec = config.audio_codec;
    if (codec.empty()) codec = config.audio_bits == 16 ? "pcm_s16le" : "pcm_f32le";
    {
        std::lock_guard<std::mutex> il(d.info_mutex);
        d.format = codec + " " + std::to_string(config.audio_channels) + "ch " + std::to_string(config.audio_rate) + " Hz";
    }
    if (codec == "pcm_f32le") d.sample = Sample::F32;
    else if (codec == "pcm_s16le") d.sample = Sample::S16;
    else if (codec == "pcm_s32le") d.sample = Sample::S32;
    else {
        d.set_info("Unsupported audio format", "The sound format " + codec + " is not supported.");
        return false;
    }
    if (config.audio_channels < 1 || config.audio_rate < 8000 || config.audio_rate > 192000) {
        d.set_info("Unsupported audio format", "The sound format is not usable.");
        return false;
    }
    d.rate = config.audio_rate;
    d.channels = config.audio_channels;
    d.url = url;

    std::string error;
    if (!sdl_audio_acquire(error)) {
        d.set_info("Audio unavailable", "Sound output could not start: " + error);
        return false;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = d.rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;   // ~10.7 ms: each buffer is latency, and the minimum buffer is two of them
    want.callback = &Impl::callback;
    want.userdata = &d;
    d.dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (d.dev == 0) {
        std::string why = SDL_GetError();
        sdl_audio_release();
        d.set_info("Audio device failed", "The sound device could not be opened: " + why);
        return false;
    }
    d.device_frames = have.samples > 0 ? have.samples : 512;
    std::string name = "Default output device";
#if SDL_VERSION_ATLEAST(2, 24, 0)
    char* dev_name = nullptr;
    SDL_AudioSpec dspec;
    if (SDL_GetDefaultAudioInfo(&dev_name, &dspec, 0) == 0 && dev_name) {
        name = dev_name;
        SDL_free(dev_name);
    }
#endif
    {
        std::lock_guard<std::mutex> rl(d.ring_mutex);
        d.cap = (size_t)(d.rate * kRingSeconds);
        d.ring.assign(d.cap * 2, 0.0f);
        d.rd = d.count = 0;
        // Enough for one callback at the fastest resampling ratio, plus slack.
        d.scratch.assign(((size_t)d.device_frames * 2 + 16) * 2, 0.0f);
    }
    {
        std::lock_guard<std::mutex> il(d.info_mutex);
        d.device = name;
        d.state = "Buffering";
    }
    d.net_stop.store(false);
    d.net = std::thread([&d] { d.network_loop(); });
    d.active.store(true);
    SDL_PauseAudioDevice(d.dev, 0);
    return true;
}

void AudioPlayer::stop_session()
{
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.session_mutex);
    if (!d.active.load() && !d.net.joinable() && d.dev == 0) return;
    d.net_stop.store(true);
    if (d.net.joinable()) d.net.join();
    if (d.dev) {
        SDL_CloseAudioDevice(d.dev);   // waits for a running callback
        d.dev = 0;
        sdl_audio_release();
    }
    {
        std::lock_guard<std::mutex> rl(d.ring_mutex);
        d.count = d.rd = 0;
        d.primed = false;
        d.ring.clear();
        d.ring.shrink_to_fit();
    }
    d.active.store(false);
    d.streaming.store(false);
    d.playout.store(-1.0);
    std::lock_guard<std::mutex> il(d.info_mutex);
    d.state = "Stopped";
}

void AudioPlayer::set_auto_floor_ms(int ms)
{
    impl_->floor_ms.store(std::clamp(ms, 30, 400));
    if (!impl_->active.load()) impl_->auto_target_ms.store(std::max(kStartTargetMs, (double)impl_->floor_ms.load()));
}

double AudioPlayer::playout_ms() const
{
    return impl_->active.load() ? impl_->playout.load() : -1.0;
}

void AudioPlayer::set_sink(Sink sink)
{
    std::lock_guard<std::mutex> lock(impl_->sink_mutex);
    impl_->sink = std::move(sink);
}

}
