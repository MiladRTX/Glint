#include "media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <fstream>

namespace fs = std::filesystem;

namespace glint {
namespace {

std::string av_text(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::string stamp() {
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

std::string lower_ext(const fs::path& path) {
    std::string ext = path.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

} // namespace

std::string default_media_dir() {
    const char* profile = std::getenv("USERPROFILE");
    if (!profile || !*profile) profile = std::getenv("HOME");
    fs::path base = profile ? fs::path(profile) : fs::current_path();
    fs::path dir = base / "Videos" / "Glint";
    // Recordings from before the rename: Videos\Screenlink becomes Videos\Glint.
    // If it cannot be renamed (a file in it is open), keep using it.
    static bool checked = false;
    static fs::path old_dir;
    if (!checked) {
        checked = true;
        std::error_code ec;
        fs::path old = base / "Videos" / "Screenlink";
        if (!fs::exists(dir, ec) && fs::is_directory(old, ec)) {
            fs::rename(old, dir, ec);
            if (ec) old_dir = old;
        }
    }
    return (old_dir.empty() ? dir : old_dir).u8string();
}

bool load_image_rgba(const std::string& path, std::vector<uint8_t>& rgba, int& width,
                     int& height, std::string& error) {
    av_log_set_level(AV_LOG_QUIET);
    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        error = "Could not open the picture: " + av_text(ret);
        return false;
    }
    bool ok = false;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    SwsContext* sws = nullptr;
    do {
        if (avformat_find_stream_info(fmt, nullptr) < 0) { error = "Unreadable picture."; break; }
        int index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index < 0) { error = "No picture data."; break; }
        AVStream* stream = fmt->streams[index];
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) { error = "Unsupported picture format."; break; }
        ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, stream->codecpar);
        if (avcodec_open2(ctx, codec, nullptr) < 0) { error = "Unsupported picture format."; break; }
        bool got = false;
        while (!got && av_read_frame(fmt, packet) >= 0) {
            if (packet->stream_index == index && avcodec_send_packet(ctx, packet) >= 0)
                got = avcodec_receive_frame(ctx, frame) >= 0;
            av_packet_unref(packet);
        }
        if (!got) {
            avcodec_send_packet(ctx, nullptr);
            got = avcodec_receive_frame(ctx, frame) >= 0;
        }
        if (!got) { error = "Could not decode the picture."; break; }
        width = frame->width;
        height = frame->height;
        sws = sws_getContext(width, height, (AVPixelFormat)frame->format, width, height,
                             AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) { error = "Could not convert the picture."; break; }
        rgba.assign((size_t)width * height * 4, 0);
        uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {width * 4, 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, height, dst, dst_stride);
        ok = true;
    } while (false);
    if (sws) sws_freeContext(sws);
    av_packet_free(&packet);
    av_frame_free(&frame);
    if (ctx) avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return ok;
}

bool save_screenshot(const std::string& dir, const std::vector<uint8_t>& frame, int width,
                     int height, std::string& out_path, std::string& error) {
    if (width <= 0 || height <= 0 || frame.size() < (size_t)width * height * 4) {
        error = "Screenshot could not be saved.";
        return false;
    }
    fs::path folder = fs::u8path(dir);
    if (dir.empty() || !folder.is_absolute()) {
        error = "Choose an absolute destination folder for local recordings.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = "Cannot create screenshot: destination already exists or is not writable.";
        return false;
    }
    fs::path file = folder / ("Glint-Screenshot-" + stamp() + ".png");
    for (int n = 2; fs::exists(file); n++)
        file = folder / ("Glint-Screenshot-" + stamp() + "-" + std::to_string(n) + ".png");

    std::vector<uint8_t> pixels(frame.begin(), frame.begin() + (size_t)width * height * 4);

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec) {
        error = "Cannot encode the PNG screenshot.";
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = width;
    ctx->height = height;
    ctx->pix_fmt = AV_PIX_FMT_RGBA;
    ctx->time_base = AVRational{1, 1};
    AVFrame* av = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    bool ok = false;
    do {
        if (avcodec_open2(ctx, codec, nullptr) < 0) { error = "Cannot encode the PNG screenshot."; break; }
        av->format = AV_PIX_FMT_RGBA;
        av->width = width;
        av->height = height;
        if (av_frame_get_buffer(av, 0) < 0) { error = "Cannot encode the PNG screenshot."; break; }
        for (int y = 0; y < height; y++)
            std::memcpy(av->data[0] + (size_t)y * av->linesize[0], &pixels[(size_t)y * width * 4],
                        (size_t)width * 4);
        av->pts = 0;
        if (avcodec_send_frame(ctx, av) < 0 || avcodec_send_frame(ctx, nullptr) < 0 ||
            avcodec_receive_packet(ctx, packet) < 0) {
            error = "Cannot encode the PNG screenshot.";
            break;
        }
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write((const char*)packet->data, packet->size);
        out.close();
        if (!out) {
            std::error_code ignore;
            fs::remove(file, ignore);
            error = "Cannot write the screenshot.";
            break;
        }
        ok = true;
    } while (false);
    av_packet_free(&packet);
    av_frame_free(&av);
    avcodec_free_context(&ctx);
    if (ok) out_path = file.u8string();
    return ok;
}

FolderWatch::~FolderWatch() { close(); }

void FolderWatch::close() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) FindCloseChangeNotification((HANDLE)handle_);
    handle_ = nullptr;
}

void FolderWatch::watch(const std::string& dir) {
    if (dir == dir_ && handle_ && handle_ != INVALID_HANDLE_VALUE) return;
    close();
    dir_ = dir;
    // A folder that doesn't exist yet can't be watched; watch() is called
    // again on the next refresh and picks it up once it does.
    std::wstring wide = fs::u8path(dir).wstring();
    HANDLE h = FindFirstChangeNotificationW(wide.c_str(), FALSE,
                                            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                                FILE_NOTIFY_CHANGE_LAST_WRITE);
    handle_ = h == INVALID_HANDLE_VALUE ? nullptr : (void*)h;
}

bool FolderWatch::changed() {
    if (!handle_) return false;
    if (WaitForSingleObject((HANDLE)handle_, 0) != WAIT_OBJECT_0) return false;
    if (!FindNextChangeNotification((HANDLE)handle_)) close();   // the folder went away: watch again later
    return true;
}

std::vector<GalleryItem> list_gallery(const std::string& dir) {
    std::vector<GalleryItem> items;
    std::error_code ec;
    fs::path folder = fs::u8path(dir);
    if (!fs::is_directory(folder, ec)) return items;

    auto file_now = fs::file_time_type::clock::now();
    auto sys_now = std::chrono::system_clock::now();

    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = lower_ext(entry.path());
        if (ext != ".mkv" && ext != ".mp4" && ext != ".png") continue;
        GalleryItem item;
        item.path = entry.path().u8string();
        item.name = entry.path().filename().u8string();
        item.video = ext != ".png";
        item.size = entry.file_size(ec);
        auto age = file_now - entry.last_write_time(ec);
        item.modified = std::chrono::duration_cast<std::chrono::seconds>(
                            (sys_now - age).time_since_epoch()).count();
        if (item.video) {
            AVFormatContext* fmt = nullptr;
            av_log_set_level(AV_LOG_QUIET);
            if (avformat_open_input(&fmt, item.path.c_str(), nullptr, nullptr) >= 0) {
                if (avformat_find_stream_info(fmt, nullptr) >= 0) {
                    if (fmt->duration > 0) item.duration = (double)fmt->duration / AV_TIME_BASE;
                    for (unsigned i = 0; i < fmt->nb_streams; i++) {
                        const AVCodecParameters* p = fmt->streams[i]->codecpar;
                        if (p->codec_type == AVMEDIA_TYPE_AUDIO) item.has_audio = true;
                        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
                            item.width = p->width;
                            item.height = p->height;
                        }
                    }
                }
                avformat_close_input(&fmt);
            }
        } else {
            // A PNG's size is in its header: the IHDR chunk right after the
            // 8-byte signature holds width and height (big-endian) at 16..23.
            std::ifstream png(entry.path(), std::ios::binary);
            unsigned char head[24] = {};
            if (png.read(reinterpret_cast<char*>(head), sizeof(head)) && head[1] == 'P' && head[2] == 'N' &&
                head[3] == 'G' && std::memcmp(head + 12, "IHDR", 4) == 0) {
                auto be32 = [&](int at) { return (int)((head[at] << 24) | (head[at + 1] << 16) | (head[at + 2] << 8) | head[at + 3]); };
                item.width = be32(16);
                item.height = be32(20);
            }
        }
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(),
              [](const GalleryItem& a, const GalleryItem& b) { return a.modified > b.modified; });
    return items;
}

bool delete_gallery_file(const std::string& path, std::string& error) {
    std::error_code ec;
    fs::path file = fs::u8path(path);
    std::string ext = lower_ext(file);
    if (ext != ".mkv" && ext != ".mp4" && ext != ".png") {
        error = "Could not delete the gallery file.";
        return false;
    }
    if (!fs::remove(file, ec) || ec) {
        error = "Could not delete the gallery file.";
        return false;
    }
    return true;
}

bool export_copy(const std::string& source, const std::string& ext, std::string& out_path,
                 std::string& error) {
    if (ext != "mp4" && ext != "mkv") {
        error = "Choose MP4 or MKV as the export format.";
        return false;
    }
    fs::path in_path = fs::u8path(source);
    fs::path out = in_path.parent_path() /
                   (in_path.stem().u8string() + "-export-" + stamp() + "." + ext);
    std::error_code same_ec;
    if (in_path == out || fs::equivalent(in_path, out, same_ec)) {
        error = "The exported file must differ from the original.";
        return false;
    }
    if (fs::exists(out)) {
        error = "The exported file already exists.";
        return false;
    }
    std::string out_utf8 = out.u8string();

    av_log_set_level(AV_LOG_QUIET);
    AVFormatContext* in = nullptr;
    AVFormatContext* dst = nullptr;
    bool ok = false;
    bool header = false;
    std::vector<int> map;
    AVPacket* packet = av_packet_alloc();
    do {
        if (avformat_open_input(&in, source.c_str(), nullptr, nullptr) < 0) {
            error = "Could not open the recording for export.";
            break;
        }
        if (avformat_find_stream_info(in, nullptr) < 0) {
            error = "Could not read the recording for export.";
            break;
        }
        if (avformat_alloc_output_context2(&dst, nullptr, ext == "mp4" ? "mp4" : "matroska",
                                           out_utf8.c_str()) < 0 || !dst) {
            error = "Could not create the exported file.";
            break;
        }
        map.assign(in->nb_streams, -1);
        int copied = 0;
        for (unsigned i = 0; i < in->nb_streams; i++) {
            AVCodecParameters* p = in->streams[i]->codecpar;
            if (p->codec_type != AVMEDIA_TYPE_VIDEO && p->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            AVStream* s = avformat_new_stream(dst, nullptr);
            if (!s || avcodec_parameters_copy(s->codecpar, p) < 0) {
                error = "Could not copy a stream to the exported file.";
                copied = -1;
                break;
            }
            s->codecpar->codec_tag = 0;
            s->time_base = in->streams[i]->time_base;
            s->avg_frame_rate = in->streams[i]->avg_frame_rate;
            map[i] = s->index;
            copied++;
        }
        if (copied < 0) break;
        if (copied == 0) {
            error = "The recording has no video or audio streams to export.";
            break;
        }
        if (avio_open(&dst->pb, out_utf8.c_str(), AVIO_FLAG_WRITE) < 0) {
            error = "Could not create the exported file.";
            break;
        }
        AVDictionary* options = nullptr;
        if (ext == "mp4") av_dict_set(&options, "movflags", "+faststart", 0);
        int ret = avformat_write_header(dst, &options);
        av_dict_free(&options);
        if (ret < 0) {
            error = "Could not start the exported file.";
            break;
        }
        header = true;
        bool write_failed = false;
        while (av_read_frame(in, packet) >= 0) {
            int target = packet->stream_index < (int)map.size() ? map[packet->stream_index] : -1;
            if (target < 0) { av_packet_unref(packet); continue; }
            av_packet_rescale_ts(packet, in->streams[packet->stream_index]->time_base,
                                 dst->streams[target]->time_base);
            packet->stream_index = target;
            packet->pos = -1;
            if (av_interleaved_write_frame(dst, packet) < 0) {
                write_failed = true;
                av_packet_unref(packet);
                break;
            }
        }
        if (write_failed) {
            error = "Could not write the exported recording.";
            break;
        }
        if (av_write_trailer(dst) < 0) {
            header = false;
            error = "Could not finish the exported recording.";
            break;
        }
        header = false;
        ok = true;
    } while (false);

    if (dst) {
        if (header) av_write_trailer(dst);
        if (dst->pb) avio_closep(&dst->pb);
        avformat_free_context(dst);
    }
    if (in) avformat_close_input(&in);
    av_packet_free(&packet);
    if (!ok) {
        std::error_code ignore;
        fs::remove(out, ignore);
        return false;
    }
    out_path = out_utf8;
    return true;
}

bool open_with_default_app(const std::string& path) {
#ifdef _WIN32
    int wide_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) return false;
    std::wstring wide((size_t)wide_len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], wide_len);
    return (INT_PTR)ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
#else
    (void)path;
    return false;
#endif
}

} // namespace glint
