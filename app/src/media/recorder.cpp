#include "recorder.h"
#include "glint/payload_client.h"
#include "convert.h"
#include "h264_util.h"
#include "log.h"
#include "slot_timeline.h"
#include "winutil.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace glint {

const char* recorder_state_name(RecorderInfo::State state)
{
    switch (state) {
    case RecorderInfo::State::Idle: return "idle";
    case RecorderInfo::State::Armed: return "waiting for a keyframe";
    case RecorderInfo::State::Recording: return "recording";
    case RecorderInfo::State::Finalizing: return "saving";
    case RecorderInfo::State::Failed: return "failed";
    }
    return "unknown";
}

namespace {

using Clock = LocalRecorder::Clock;
using State = RecorderInfo::State;

constexpr size_t kQueueLimit = 96u * 1024 * 1024;

struct Item {
    bool video = true;
    std::vector<uint8_t> video_data;
    uint32_t pts = 0;
    std::vector<float> audio;   // stereo
    Clock::time_point arrival;
    size_t bytes() const { return video_data.size() + audio.size() * sizeof(float); }
};

bool is_absolute(const std::string& s)
{
    if (s.size() >= 3 && std::isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/'))
        return true;
    return s.size() >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/');
}

bool exists(const std::string& path)
{
    return GetFileAttributesW(widen(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool make_dirs(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2) make_dirs(path.substr(0, cut));
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string base_name(const std::string& path)
{
    size_t cut = path.find_last_of("\\/");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

std::string av_error(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

}  // namespace

struct LocalRecorder::Impl {
    mutable std::mutex mutex;
    std::condition_variable wake;
    RecorderInfo info;
    std::deque<Item> queue;
    size_t queued_bytes = 0;
    bool stop_requested = false;
    std::string stop_reason;
    Clock::time_point first_video_arrival;
    int audio_rate = 0;
    std::thread writer;

    // Writer-thread state.
    AVFormatContext* fmt = nullptr;
    AVStream* vstream = nullptr;
    AVStream* astream = nullptr;
    AVCodecContext* aac = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* pkt = nullptr;
    SlotTimeline timeline;
    PtsUnwrap unwrap;
    bool audio_placed = false;
    int64_t next_sample = 0;
    std::vector<float> pend_l, pend_r;
    std::string write_error;

    void cleanup()
    {
        if (aac) avcodec_free_context(&aac);
        if (aframe) av_frame_free(&aframe);
        if (pkt) av_packet_free(&pkt);
        if (fmt) {
            if (fmt->pb) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
            fmt = nullptr;
        }
        vstream = astream = nullptr;
    }

    bool write_packet(AVPacket* p)
    {
        int rc = av_interleaved_write_frame(fmt, p);
        if (rc < 0) {
            write_error = "Writing the recording failed (" + av_error(rc) + ").";
            return false;
        }
        return true;
    }

    // Length-prefixed sample from an Annex-B access unit; false when it has no slice.
    static bool to_sample(const std::vector<uint8_t>& au, std::vector<uint8_t>& out, bool& key)
    {
        out.clear();
        bool slice = false;
        key = false;
        for_each_nal(au.data(), au.size(), [&](int type, const uint8_t* nal, size_t len) {
            if (type == 7 || type == 8 || type == 9) return;   // in avcC / not needed
            if (type >= 1 && type <= 5) slice = true;
            if (type == 5) key = true;
            uint32_t n = (uint32_t)len;
            uint8_t head[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
            out.insert(out.end(), head, head + 4);
            out.insert(out.end(), nal, nal + len);
        });
        return slice;
    }

    bool write_video(const Item& item)
    {
        std::vector<uint8_t> sample;
        bool key = false;
        if (!to_sample(item.video_data, sample, key)) return true;
        int64_t slot = timeline.place(unwrap(item.pts));
        int64_t t0 = timeline.slot_ticks(slot), t1 = timeline.slot_ticks(slot + 1);
        av_packet_unref(pkt);
        if (av_new_packet(pkt, (int)sample.size()) < 0) {
            write_error = "Out of memory while recording.";
            return false;
        }
        std::memcpy(pkt->data, sample.data(), sample.size());
        pkt->stream_index = vstream->index;
        pkt->pts = pkt->dts = t0;
        pkt->duration = t1 - t0;
        if (key) pkt->flags |= AV_PKT_FLAG_KEY;
        av_packet_rescale_ts(pkt, AVRational{1, 90000}, vstream->time_base);
        if (!write_packet(pkt)) return false;
        std::lock_guard<std::mutex> lock(mutex);
        info.video_frames++;
        info.seconds = (double)t0 / 90000.0;
        return true;
    }

    bool drain_encoder()
    {
        for (;;) {
            int rc = avcodec_receive_packet(aac, pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
            if (rc < 0) {
                write_error = "Encoding the sound failed (" + av_error(rc) + ").";
                return false;
            }
            // The encoder's priming packet can fall before zero when sound starts at once.
            if (pkt->pts < 0) {
                av_packet_unref(pkt);
                continue;
            }
            pkt->stream_index = astream->index;
            av_packet_rescale_ts(pkt, aac->time_base, astream->time_base);
            if (!write_packet(pkt)) return false;
        }
    }

    bool encode_pending(bool final)
    {
        const size_t fs = (size_t)aac->frame_size;
        if (final && !pend_l.empty() && pend_l.size() % fs) {
            size_t pad = fs - pend_l.size() % fs;
            pend_l.insert(pend_l.end(), pad, 0.0f);
            pend_r.insert(pend_r.end(), pad, 0.0f);
        }
        size_t done = 0;
        while (pend_l.size() - done >= fs) {
            if (av_frame_make_writable(aframe) < 0) {
                write_error = "Out of memory while recording.";
                return false;
            }
            std::memcpy(aframe->data[0], pend_l.data() + done, fs * sizeof(float));
            std::memcpy(aframe->data[1], pend_r.data() + done, fs * sizeof(float));
            aframe->pts = next_sample;
            next_sample += (int64_t)fs;
            done += fs;
            int rc = avcodec_send_frame(aac, aframe);
            if (rc < 0) {
                write_error = "Encoding the sound failed (" + av_error(rc) + ").";
                return false;
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                info.audio_frames += fs;
            }
            if (!drain_encoder()) return false;
        }
        pend_l.erase(pend_l.begin(), pend_l.begin() + (ptrdiff_t)done);
        pend_r.erase(pend_r.begin(), pend_r.begin() + (ptrdiff_t)done);
        if (final) {
            avcodec_send_frame(aac, nullptr);
            if (!drain_encoder()) return false;
        }
        return true;
    }

    bool write_audio(const Item& item)
    {
        if (!aac) return true;
        size_t frames = item.audio.size() / 2;
        if (!audio_placed) {
            // The first block goes where it arrived relative to the first video frame.
            double since = std::chrono::duration<double>(item.arrival - first_video_arrival).count();
            double start = since - (double)frames / audio_rate;
            next_sample = std::max<int64_t>(0, (int64_t)std::llround(start * audio_rate));
            audio_placed = true;
        }
        for (size_t i = 0; i < frames; i++) {
            pend_l.push_back(item.audio[i * 2]);
            pend_r.push_back(item.audio[i * 2 + 1]);
        }
        return encode_pending(false);
    }

    void update_bytes()
    {
        int64_t at = fmt && fmt->pb ? avio_tell(fmt->pb) : 0;
        std::lock_guard<std::mutex> lock(mutex);
        if (at > 0) info.bytes = (uint64_t)at;
    }

    void run()
    {
        bool failed = false;
        bool got_video = false;
        for (;;) {
            Item item;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stop_requested || !queue.empty(); });
                if (queue.empty()) break;   // stop requested and drained
                item = std::move(queue.front());
                queue.pop_front();
                queued_bytes -= item.bytes();
            }
            if (failed) continue;
            bool ok = item.video ? write_video(item) : (got_video ? write_audio(item) : true);
            if (item.video) got_video = got_video || ok;
            if (!ok) {
                failed = true;
                std::lock_guard<std::mutex> lock(mutex);
                info.state = State::Finalizing;   // stop accepting data at once
                queue.clear();
                queued_bytes = 0;
                stop_requested = true;
            }
            update_bytes();
        }
        finish(failed, got_video);
    }

    void finish(bool failed, bool got_video)
    {
        std::string path, reason;
        {
            std::lock_guard<std::mutex> lock(mutex);
            path = info.path;
            reason = stop_reason;
        }
        if (!got_video && !failed) {
            // Stopped before the first keyframe: nothing worth keeping.
            cleanup();
            DeleteFileW(widen(path).c_str());
            std::lock_guard<std::mutex> lock(mutex);
            info.state = State::Idle;
            info.path.clear();
            info.bytes = 0;
            info.message = "Recording stopped before the first keyframe; nothing was saved.";
            logf("recording: no keyframe arrived, %s removed", path.c_str());
            return;
        }
        if (!failed && aac && !encode_pending(true)) failed = true;
        int rc = av_write_trailer(fmt);
        if (rc < 0 && !failed) {
            failed = true;
            write_error = "Finishing the recording failed (" + av_error(rc) + ").";
        }
        if (fmt->pb) avio_flush(fmt->pb);
        cleanup();

        uint64_t size = 0;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(widen(path).c_str(), GetFileExInfoStandard, &fa))
            size = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;

        std::lock_guard<std::mutex> lock(mutex);
        info.bytes = size;
        if (failed) {
            info.state = State::Failed;
            info.error = write_error.empty() ? "The recording could not be written." : write_error;
            info.message = info.error;
            logf("recording: failed: %s (%s)", info.error.c_str(), path.c_str());
        } else {
            info.state = State::Idle;
            info.message = "Saved " + base_name(path) + (reason.empty() ? "." : " (" + reason + ").");
            logf("recording: saved %s, %llu frames, %.1f s", path.c_str(), (unsigned long long)info.video_frames,
                 info.seconds);
        }
    }
};

LocalRecorder::LocalRecorder() : impl_(new Impl) {}

LocalRecorder::~LocalRecorder()
{
    stop(std::string());
    wait();
}

bool LocalRecorder::start(const std::string& dir, const StreamConfig& config, bool with_audio, int audio_rate,
                          std::string& error)
{
    Impl& d = *impl_;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        State s = d.info.state;
        if (s == State::Armed || s == State::Recording) {
            error = "A recording is already running.";
            return false;
        }
        if (s == State::Finalizing) {
            error = "The last recording is still being saved.";
            return false;
        }
    }
    wait();   // the previous writer has finished; reap it

    if (!is_absolute(dir)) {
        error = "The recordings folder must be a full path.";
        return false;
    }
    if (!make_dirs(widen(dir))) {
        error = "The recordings folder could not be created.";
        return false;
    }
    std::vector<uint8_t> sps, pps;
    hex_to_bytes(config.sps, sps);
    hex_to_bytes(config.pps, pps);
    if (!valid_param_sets(sps, pps) || config.width <= 0 || config.height <= 0) {
        error = "The console's stream description is incomplete, so it can't be recorded.";
        return false;
    }

    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    std::string folder = dir;
    if (folder.back() != '\\' && folder.back() != '/') folder += '\\';
    std::string path = folder + "Glint-" + stamp + ".mkv";
    for (int n = 2; exists(path); n++) path = folder + "Glint-" + stamp + "-" + std::to_string(n) + ".mkv";

    ffmpeg_setup();
    d.cleanup();
    d.timeline = SlotTimeline(config.fps > 0 ? config.fps : 60);
    d.unwrap = PtsUnwrap();
    d.audio_placed = false;
    d.next_sample = 0;
    d.pend_l.clear();
    d.pend_r.clear();
    d.write_error.clear();

    auto fail = [&](const std::string& why) {
        bool opened = d.fmt && d.fmt->pb;
        d.cleanup();
        if (opened) DeleteFileW(widen(path).c_str());
        error = why;
        return false;
    };

    if (avformat_alloc_output_context2(&d.fmt, nullptr, "matroska", path.c_str()) < 0 || !d.fmt)
        return fail("The recording file could not be prepared.");
    d.pkt = av_packet_alloc();

    // Video: H.264 copied, with an avcC built from the console's SPS/PPS.
    d.vstream = avformat_new_stream(d.fmt, nullptr);
    if (!d.vstream || !d.pkt) return fail("Out of memory while starting the recording.");
    std::vector<uint8_t> avcc = {1, sps[1], sps[2], sps[3], 0xFF, 0xE1, (uint8_t)(sps.size() >> 8),
                                 (uint8_t)sps.size()};
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.push_back(1);
    avcc.push_back((uint8_t)(pps.size() >> 8));
    avcc.push_back((uint8_t)pps.size());
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    AVCodecParameters* vp = d.vstream->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = AV_CODEC_ID_H264;
    vp->width = config.width;
    vp->height = config.height;
    vp->extradata = (uint8_t*)av_mallocz(avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!vp->extradata) return fail("Out of memory while starting the recording.");
    std::memcpy(vp->extradata, avcc.data(), avcc.size());
    vp->extradata_size = (int)avcc.size();
    int fps = d.timeline.fps();
    d.vstream->time_base = AVRational{1, 90000};
    d.vstream->avg_frame_rate = AVRational{fps, 1};
    d.vstream->r_frame_rate = AVRational{fps, 1};

    // Sound: AAC-LC stereo at the stream's rate.
    bool audio_on = with_audio && audio_rate > 0;
    if (audio_on) {
        const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        d.aac = enc ? avcodec_alloc_context3(enc) : nullptr;
        if (!d.aac) return fail("The AAC encoder is not available.");
        d.aac->sample_fmt = AV_SAMPLE_FMT_FLTP;
        d.aac->sample_rate = audio_rate;
        av_channel_layout_default(&d.aac->ch_layout, 2);
        d.aac->bit_rate = 192000;
        d.aac->profile = AV_PROFILE_AAC_LOW;
        d.aac->time_base = AVRational{1, audio_rate};
        if (d.fmt->oformat->flags & AVFMT_GLOBALHEADER) d.aac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (avcodec_open2(d.aac, enc, nullptr) < 0) return fail("The AAC encoder could not be opened.");
        d.astream = avformat_new_stream(d.fmt, nullptr);
        if (!d.astream || avcodec_parameters_from_context(d.astream->codecpar, d.aac) < 0)
            return fail("Out of memory while starting the recording.");
        d.astream->time_base = AVRational{1, audio_rate};
        d.aframe = av_frame_alloc();
        if (!d.aframe) return fail("Out of memory while starting the recording.");
        d.aframe->format = AV_SAMPLE_FMT_FLTP;
        d.aframe->sample_rate = audio_rate;
        d.aframe->nb_samples = d.aac->frame_size;
        av_channel_layout_copy(&d.aframe->ch_layout, &d.aac->ch_layout);
        if (av_frame_get_buffer(d.aframe, 0) < 0) return fail("Out of memory while starting the recording.");
    }

    int rc = avio_open(&d.fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) {
        d.cleanup();
        error = "The recording file could not be created (" + av_error(rc) + ").";
        return false;
    }
    d.fmt->flush_packets = 1;               // what is written survives a crash
    d.fmt->max_interleave_delta = 1000000;  // never hold packets back for long
    rc = avformat_write_header(d.fmt, nullptr);
    if (rc < 0) return fail("The recording header could not be written (" + av_error(rc) + ").");

    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.info = RecorderInfo();
        d.info.state = State::Armed;
        d.info.path = path;
        d.info.message = "Waiting for a keyframe.";
        d.info.has_audio = audio_on;
        d.info.width = config.width;
        d.info.height = config.height;
        d.queue.clear();
        d.queued_bytes = 0;
        d.stop_requested = false;
        d.stop_reason.clear();
        d.audio_rate = audio_rate;
    }
    d.update_bytes();
    d.writer = std::thread([&d] { d.run(); });
    logf("recording: started %s (%dx%d, %s)", path.c_str(), config.width, config.height,
         audio_on ? "with sound" : "no sound");
    return true;
}

void LocalRecorder::stop(const std::string& reason)
{
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.info.state != State::Armed && d.info.state != State::Recording) return;
    d.info.state = State::Finalizing;
    d.info.message = "Saving the recording.";
    d.stop_reason = reason;
    d.stop_requested = true;
    d.wake.notify_all();
}

void LocalRecorder::wait()
{
    if (impl_->writer.joinable()) impl_->writer.join();
}

void LocalRecorder::push_video(const uint8_t* annexb, size_t len, uint32_t pts, Clock::time_point arrival)
{
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.info.state == State::Armed) {
        if (!has_idr(annexb, len)) return;   // the file starts at a keyframe
        d.info.state = State::Recording;
        d.info.message = "Recording.";
        d.first_video_arrival = arrival;
    }
    if (d.info.state != State::Recording) return;
    if (d.queued_bytes + len > kQueueLimit) {
        d.info.dropped++;
        return;
    }
    Item item;
    item.video = true;
    item.video_data.assign(annexb, annexb + len);
    item.pts = pts;
    item.arrival = arrival;
    d.queued_bytes += item.bytes();
    d.queue.push_back(std::move(item));
    d.wake.notify_one();
}

void LocalRecorder::push_audio(const float* stereo, size_t frames, int rate, Clock::time_point arrival)
{
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.info.state != State::Recording || !d.info.has_audio || rate != d.audio_rate || frames == 0) return;
    if (d.queued_bytes + frames * 2 * sizeof(float) > kQueueLimit) {
        d.info.dropped++;
        return;
    }
    Item item;
    item.video = false;
    item.audio.assign(stereo, stereo + frames * 2);
    item.arrival = arrival;
    d.queued_bytes += item.bytes();
    d.queue.push_back(std::move(item));
    d.wake.notify_one();
}

RecorderInfo LocalRecorder::info() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->info;
}

RecorderInfo::State LocalRecorder::state() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->info.state;
}

}
