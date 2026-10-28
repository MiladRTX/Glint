#include "recorder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace glint {
namespace {

constexpr size_t kMaxQueuedBytes = 96u * 1024u * 1024u;
constexpr int kAudioBitrate = 192000;

std::string av_error_text(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> hex_bytes(const std::string& text) {
    std::vector<uint8_t> out;
    if (text.size() % 2) return out;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        int hi = hex_value(text[i]);
        int lo = hex_value(text[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return out;
}

// avcC (AVCDecoderConfigurationRecord) with 4-byte NAL lengths.
std::vector<uint8_t> build_avcc(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> out;
    out.push_back(1);
    out.push_back(sps[1]);
    out.push_back(sps[2]);
    out.push_back(sps[3]);
    out.push_back(0xFF);   // reserved + lengthSizeMinusOne = 3
    out.push_back(0xE1);   // reserved + 1 SPS
    out.push_back((uint8_t)(sps.size() >> 8));
    out.push_back((uint8_t)(sps.size() & 0xff));
    out.insert(out.end(), sps.begin(), sps.end());
    out.push_back(1);      // 1 PPS
    out.push_back((uint8_t)(pps.size() >> 8));
    out.push_back((uint8_t)(pps.size() & 0xff));
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

// Annex-B -> length-prefixed. SPS/PPS/AUD are dropped (they live in avcC).
// Returns false when the unit has no slice data.
bool annexb_to_avcc(const uint8_t* data, size_t len, std::vector<uint8_t>& out, bool& key) {
    key = false;
    out.clear();
    out.reserve(len + 16);

    struct Nal { size_t begin, end; };
    std::vector<Nal> nals;
    size_t i = 0;
    size_t start = std::string::npos;
    while (i + 3 <= len) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (start != std::string::npos) {
                size_t end = i;
                while (end > start && data[end - 1] == 0) end--;   // trailing zero of a 4-byte code
                nals.push_back({start, end});
            }
            start = i + 3;
            i += 3;
        } else {
            i++;
        }
    }
    if (start != std::string::npos && start < len) nals.push_back({start, len});

    bool slice = false;
    for (const Nal& nal : nals) {
        if (nal.end <= nal.begin) continue;
        int type = data[nal.begin] & 0x1f;
        if (type == 7 || type == 8 || type == 9) continue;
        if (type == 5) key = true;
        if (type >= 1 && type <= 5) slice = true;
        uint32_t size = (uint32_t)(nal.end - nal.begin);
        out.push_back((uint8_t)(size >> 24));
        out.push_back((uint8_t)(size >> 16));
        out.push_back((uint8_t)(size >> 8));
        out.push_back((uint8_t)size);
        out.insert(out.end(), data + nal.begin, data + nal.end);
    }
    return slice;
}

std::string timestamp_name() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &local);
    return buffer;
}

} // namespace

const char* recorder_state_name(RecorderInfo::State state) {
    switch (state) {
    case RecorderInfo::State::Idle: return "Not recording on PC";
    case RecorderInfo::State::Armed: return "Waiting for keyframe";
    case RecorderInfo::State::Recording: return "Recording locally";
    case RecorderInfo::State::Finalizing: return "Finalizing local recording...";
    case RecorderInfo::State::Failed: return "Recording failed";
    }
    return "";
}

LocalRecorder::~LocalRecorder() {
    stop();
    wait();
}

bool LocalRecorder::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_.state == RecorderInfo::State::Armed ||
           info_.state == RecorderInfo::State::Recording ||
           info_.state == RecorderInfo::State::Finalizing;
}

RecorderInfo LocalRecorder::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

void LocalRecorder::set_message(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    info_.message = text;
}

void LocalRecorder::set_failed(const std::string& text) {
    failed_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (info_.error.empty()) info_.error = text;
    capturing_.store(false);
    // End the session: the writer loop finalizes and reports Failed.
    stopping_ = true;
    info_.state = RecorderInfo::State::Finalizing;
    info_.message = "Recording failed. Finalizing file...";
    cv_.notify_all();
}

bool LocalRecorder::start(const std::string& dir, const StreamConfig& config, bool with_audio,
                          std::string& error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (info_.state == RecorderInfo::State::Finalizing) {
            error = "Wait for the current local recording to finish saving.";
            return false;
        }
        if (info_.state == RecorderInfo::State::Armed ||
            info_.state == RecorderInfo::State::Recording) {
            error = "Already recording on this PC.";
            return false;
        }
    }
    if (thread_.joinable()) thread_.join();

    namespace fs = std::filesystem;
    fs::path folder = fs::u8path(dir);
    if (dir.empty() || !folder.is_absolute()) {
        error = "Choose an absolute destination folder for local recordings.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec || !fs::is_directory(folder)) {
        error = "Could not create the local recording.";
        return false;
    }

    std::vector<uint8_t> sps = hex_bytes(config.sps);
    std::vector<uint8_t> pps = hex_bytes(config.pps);
    if (sps.size() < 4 || pps.empty() || config.width <= 0 || config.height <= 0) {
        error = "The console has not published a valid H.264 configuration yet.";
        return false;
    }

    std::string base = "Glint-" + timestamp_name();
    fs::path file = folder / (base + ".mkv");
    for (int n = 2; fs::exists(file); n++)
        file = folder / (base + "-" + std::to_string(n) + ".mkv");
    std::string path = file.u8string();

    auto fail = [&](const std::string& text, int code = 0) {
        error = code ? text + " (" + av_error_text(code) + ")" : text;
        if (aac_) avcodec_free_context(&aac_);
        if (fmt_) {
            if (fmt_->pb) avio_closep(&fmt_->pb);
            avformat_free_context(fmt_);
            fmt_ = nullptr;
        }
        video_stream_ = audio_stream_ = nullptr;
        header_written_ = false;
        std::error_code ignore;
        fs::remove(file, ignore);
        return false;
    };

    int ret = avformat_alloc_output_context2(&fmt_, nullptr, "matroska", path.c_str());
    if (ret < 0 || !fmt_) return fail("Could not create the local recording.", ret);

    fps_ = config.fps > 0 ? config.fps : 60;
    video_stream_ = avformat_new_stream(fmt_, nullptr);
    if (!video_stream_) return fail("Could not create the local recording.");
    AVCodecParameters* vp = video_stream_->codecpar;
    vp->codec_type = AVMEDIA_TYPE_VIDEO;
    vp->codec_id = AV_CODEC_ID_H264;
    vp->width = config.width;
    vp->height = config.height;
    vp->format = AV_PIX_FMT_YUV420P;
    std::vector<uint8_t> avcc = build_avcc(sps, pps);
    vp->extradata = (uint8_t*)av_mallocz(avcc.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!vp->extradata) return fail("Could not create the local recording.");
    std::memcpy(vp->extradata, avcc.data(), avcc.size());
    vp->extradata_size = (int)avcc.size();
    video_stream_->time_base = AVRational{1, 90000};
    video_stream_->avg_frame_rate = AVRational{fps_, 1};

    bool want_audio = with_audio && config.has_audio;
    audio_rate_ = config.audio_rate > 0 ? config.audio_rate : 48000;
    if (want_audio) {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec) return fail("FFmpeg AAC encoder unavailable.");
        aac_ = avcodec_alloc_context3(codec);
        if (!aac_) return fail("Could not create the local recording.");
        aac_->sample_rate = audio_rate_;
        aac_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&aac_->ch_layout, 2);
        aac_->bit_rate = kAudioBitrate;
        aac_->time_base = AVRational{1, audio_rate_};
        if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) aac_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        ret = avcodec_open2(aac_, codec, nullptr);
        if (ret < 0) return fail("Could not open the AAC encoder.", ret);
        audio_stream_ = avformat_new_stream(fmt_, nullptr);
        if (!audio_stream_) return fail("Could not create the local recording.");
        ret = avcodec_parameters_from_context(audio_stream_->codecpar, aac_);
        if (ret < 0) return fail("Could not create the local recording.", ret);
        audio_stream_->time_base = AVRational{1, audio_rate_};
    }

    ret = avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (ret < 0) return fail("Could not create the local recording.", ret);
    ret = avformat_write_header(fmt_, nullptr);
    if (ret < 0) return fail("Could not start the local recording.", ret);
    header_written_ = true;

    failed_ = false;
    started_ = false;
    last_raw_ = 0;
    unwrapped_ = 0;
    first_pts_ = 0;
    last_dts_ = -1;
    grid_.reset(fps_);
    audio_aligned_ = false;
    audio_next_pts_ = 0;
    audio_fifo_.clear();
    bytes_written_ = 0;
    queue_.clear();
    queued_bytes_ = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        info_ = RecorderInfo{};
        info_.state = RecorderInfo::State::Armed;
        info_.path = path;
        info_.has_audio = audio_stream_ != nullptr;
        info_.width = config.width;
        info_.height = config.height;
        info_.message = "Waiting for the next keyframe...";
        stopping_ = false;
        stop_reason_.clear();
    }
    capturing_.store(true);
    thread_ = std::thread(&LocalRecorder::run, this);
    return true;
}

void LocalRecorder::stop(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (info_.state != RecorderInfo::State::Armed &&
        info_.state != RecorderInfo::State::Recording)
        return;
    capturing_.store(false);
    stopping_ = true;
    stop_reason_ = reason;
    info_.state = RecorderInfo::State::Finalizing;
    info_.message = reason.empty() ? "Finalizing local recording..." : reason;
    cv_.notify_all();
}

void LocalRecorder::wait() {
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) thread_.join();
}

void LocalRecorder::push_video(const uint8_t* annexb, size_t len, uint32_t pts90k) {
    if (!capturing_.load(std::memory_order_relaxed)) return;
    Item item;
    item.video = true;
    item.raw_pts = pts90k;
    item.arrival = std::chrono::steady_clock::now();
    if (!annexb_to_avcc(annexb, len, item.bytes, item.key)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capturing_.load()) return;
    if (queued_bytes_ + item.bytes.size() > kMaxQueuedBytes) {
        info_.dropped++;
        return;
    }
    queued_bytes_ += item.bytes.size();
    queue_.push_back(std::move(item));
    cv_.notify_one();
}

void LocalRecorder::on_audio(const float* stereo, size_t frames, int) {
    if (!capturing_.load(std::memory_order_relaxed) || !audio_stream_) return;
    Item item;
    item.video = false;
    item.arrival = std::chrono::steady_clock::now();
    item.samples.assign(stereo, stereo + frames * 2);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!capturing_.load()) return;
    size_t bytes = item.samples.size() * sizeof(float);
    if (queued_bytes_ + bytes > kMaxQueuedBytes) {
        info_.dropped++;
        return;
    }
    queued_bytes_ += bytes;
    queue_.push_back(std::move(item));
    cv_.notify_one();
}

void LocalRecorder::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        cv_.wait(lock, [&] { return !queue_.empty() || stopping_; });
        if (queue_.empty() && stopping_) break;
        Item item = std::move(queue_.front());
        queue_.pop_front();
        queued_bytes_ -= item.video ? item.bytes.size() : item.samples.size() * sizeof(float);
        lock.unlock();
        if (!failed_) {
            if (item.video) write_video(item);
            else write_audio(item);
        }
        lock.lock();
    }
    lock.unlock();
    finalize();
}

void LocalRecorder::write_packet_error(int code, const char* what) {
    set_failed(std::string("Could not write the local recording: ") + what + " (" +
               av_error_text(code) + ")");
}

void LocalRecorder::write_video(Item& item) {
    // Unwrap the 32-bit 90 kHz timestamp.
    if (!started_) {
        if (!item.key) return;   // begin at a keyframe
        started_ = true;
        last_raw_ = item.raw_pts;
        unwrapped_ = 0;
        first_pts_ = 0;
        video_start_wall_ = item.arrival;
        std::lock_guard<std::mutex> lock(mutex_);
        if (info_.state == RecorderInfo::State::Armed) {
            info_.state = RecorderInfo::State::Recording;
            info_.message = "Recording locally";
        }
    } else {
        int32_t delta = (int32_t)(item.raw_pts - last_raw_);
        last_raw_ = item.raw_pts;
        unwrapped_ += delta;
    }
    // The console's own stamps jitter: place the frame on an exact 1/fps
    // timeline instead (frame_grid.h).
    int64_t pts = grid_.ticks(grid_.place(unwrapped_ - first_pts_));
    if (pts <= last_dts_) pts = last_dts_ + 1;
    last_dts_ = pts;

    AVPacket* packet = av_packet_alloc();
    if (!packet) return;
    if (av_new_packet(packet, (int)item.bytes.size()) < 0) {
        av_packet_free(&packet);
        return;
    }
    std::memcpy(packet->data, item.bytes.data(), item.bytes.size());
    packet->pts = packet->dts = pts;
    packet->duration = 90000 / fps_;
    packet->stream_index = video_stream_->index;
    if (item.key) packet->flags |= AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(packet, AVRational{1, 90000}, video_stream_->time_base);
    size_t size = item.bytes.size();
    int ret = av_interleaved_write_frame(fmt_, packet);
    av_packet_free(&packet);
    if (ret < 0) {
        write_packet_error(ret, "video");
        return;
    }
    bytes_written_ += size;
    std::lock_guard<std::mutex> lock(mutex_);
    info_.video_frames++;
    info_.bytes = bytes_written_;
    info_.seconds = (double)pts / 90000.0;
}

void LocalRecorder::write_audio(Item& item) {
    if (!started_ || !audio_stream_ || !aac_) return;
    size_t frames = item.samples.size() / 2;
    if (!audio_aligned_) {
        // Place the first audio sample relative to the first video frame using
        // arrival times (both streams share the same network path).
        double duration = (double)frames / audio_rate_;
        double offset = std::chrono::duration<double>(item.arrival - video_start_wall_).count() -
                        duration;
        audio_next_pts_ = offset > 0 ? (int64_t)std::llround(offset * audio_rate_) : 0;
        audio_aligned_ = true;
    }
    audio_fifo_.insert(audio_fifo_.end(), item.samples.begin(), item.samples.end());
    encode_audio(false);
}

void LocalRecorder::encode_audio(bool flush) {
    if (!aac_ || !audio_stream_) return;
    const int frame_size = aac_->frame_size > 0 ? aac_->frame_size : 1024;
    size_t consumed = 0;

    auto drain = [&]() {
        AVPacket* packet = av_packet_alloc();
        for (;;) {
            int ret = avcodec_receive_packet(aac_, packet);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                write_packet_error(ret, "AAC encode");
                break;
            }
            packet->stream_index = audio_stream_->index;
            av_packet_rescale_ts(packet, aac_->time_base, audio_stream_->time_base);
            ret = av_interleaved_write_frame(fmt_, packet);
            if (ret < 0) {
                write_packet_error(ret, "audio");
                break;
            }
            bytes_written_ += (uint64_t)packet->size;
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    };

    auto send = [&](const float* interleaved, int available_frames) {
        AVFrame* frame = av_frame_alloc();
        frame->nb_samples = frame_size;
        frame->format = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&frame->ch_layout, 2);
        frame->sample_rate = audio_rate_;
        if (av_frame_get_buffer(frame, 0) < 0) {
            av_frame_free(&frame);
            return false;
        }
        float* left = (float*)frame->extended_data[0];
        float* right = (float*)frame->extended_data[1];
        for (int i = 0; i < frame_size; i++) {
            if (i < available_frames) {
                left[i] = interleaved[i * 2];
                right[i] = interleaved[i * 2 + 1];
            } else {
                left[i] = right[i] = 0.f;
            }
        }
        frame->pts = audio_next_pts_;
        audio_next_pts_ += frame_size;
        int ret = avcodec_send_frame(aac_, frame);
        av_frame_free(&frame);
        if (ret < 0) {
            write_packet_error(ret, "AAC encode");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            info_.audio_frames += (uint64_t)std::min(frame_size, available_frames);
        }
        drain();
        return !failed_;
    };

    while (!failed_ && audio_fifo_.size() - consumed >= (size_t)frame_size * 2) {
        if (!send(audio_fifo_.data() + consumed, frame_size)) break;
        consumed += (size_t)frame_size * 2;
    }
    if (flush && !failed_) {
        size_t rest = (audio_fifo_.size() - consumed) / 2;
        if (rest > 0 && send(audio_fifo_.data() + consumed, (int)rest)) consumed += rest * 2;
        avcodec_send_frame(aac_, nullptr);
        drain();
    }
    audio_fifo_.erase(audio_fifo_.begin(), audio_fifo_.begin() + (std::ptrdiff_t)consumed);
}

void LocalRecorder::finalize() {
    if (started_ && audio_stream_ && !failed_) encode_audio(true);
    int ret = 0;
    if (header_written_) ret = av_write_trailer(fmt_);
    if (fmt_) {
        if (fmt_->pb) avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
    }
    if (aac_) avcodec_free_context(&aac_);
    video_stream_ = audio_stream_ = nullptr;
    header_written_ = false;

    std::lock_guard<std::mutex> lock(mutex_);
    capturing_.store(false);
    std::error_code ec;
    auto size = std::filesystem::file_size(std::filesystem::u8path(info_.path), ec);
    if (!ec) info_.bytes = size;
    if (!started_ && !failed_) {
        // Stopped before any keyframe arrived: nothing worth keeping.
        std::filesystem::remove(std::filesystem::u8path(info_.path), ec);
        info_.state = RecorderInfo::State::Idle;
        info_.message = "Recording stopped before the first keyframe; no file kept.";
        info_.path.clear();
    } else if (failed_ || ret < 0) {
        info_.state = RecorderInfo::State::Failed;
        if (info_.error.empty()) info_.error = "Could not finalize local recording.";
        info_.message = "Could not finalize local recording.";
    } else {
        info_.state = RecorderInfo::State::Idle;
        info_.message = stop_reason_.empty() ? "Local recording saved and finalized."
                                             : stop_reason_ + " Saved and finalized.";
    }
    stopping_ = false;
    queue_.clear();
    queued_bytes_ = 0;
}

} // namespace glint
