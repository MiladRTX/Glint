#include "media.h"
#include "convert.h"
#include "log.h"
#include "winutil.h"

#include <shellapi.h>
#include <zlib.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <mutex>

namespace glint {

namespace {

bool is_absolute(const std::string& s)
{
    if (s.size() >= 3 && std::isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/'))
        return true;
    return s.size() >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/');
}

bool path_exists(const std::wstring& p)
{
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool make_dirs(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 2) make_dirs(path.substr(0, cut));
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string with_slash(std::string dir)
{
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
    return dir;
}

std::string lower_ext(const std::string& name)
{
    size_t dot = name.find_last_of('.');
    size_t slash = name.find_last_of("\\/");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

std::string local_stamp()
{
    char buf[32];
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string av_error(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

// --- PNG writer (zlib) ----------------------------------------------------------------

void put_be32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

void png_chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t len)
{
    put_be32(out, (uint32_t)len);
    size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    if (len) out.insert(out.end(), data, data + len);
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, out.data() + at, (uInt)(len + 4));
    put_be32(out, (uint32_t)crc);
}

// RGB, 8 bits, "Sub" filter on every row.
bool encode_png(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& png)
{
    const size_t stride = (size_t)w * 3 + 1;
    std::vector<uint8_t> raw(stride * h);
    for (int y = 0; y < h; y++) {
        uint8_t* row = raw.data() + stride * y;
        const uint8_t* src = rgba + (size_t)y * w * 4;
        row[0] = 1;
        uint8_t prev[3] = {0, 0, 0};
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < 3; c++) {
                uint8_t v = src[x * 4 + c];
                row[1 + x * 3 + c] = (uint8_t)(v - prev[c]);
                prev[c] = v;
            }
        }
    }
    uLongf zlen = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zlen);
    if (compress2(z.data(), &zlen, raw.data(), (uLong)raw.size(), 4) != Z_OK) return false;

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.assign(sig, sig + 8);
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr, (uint32_t)h);
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(2);   // RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    png_chunk(png, "IHDR", ihdr.data(), ihdr.size());
    png_chunk(png, "IDAT", z.data(), zlen);
    png_chunk(png, "IEND", nullptr, 0);
    return true;
}

bool png_size(const std::wstring& path, int& w, int& h)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint8_t b[24];
    DWORD got = 0;
    bool ok = ReadFile(f, b, sizeof(b), &got, nullptr) && got == sizeof(b);
    CloseHandle(f);
    if (!ok || b[0] != 0x89 || std::memcmp(b + 12, "IHDR", 4) != 0) return false;
    w = (int)((uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19]);
    h = (int)((uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23]);
    return true;
}

void probe_video(const std::string& path, GalleryItem& item)
{
    ffmpeg_setup();
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return;
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) item.duration = fmt->duration / (double)AV_TIME_BASE;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVStream* st = fmt->streams[i];
        // MP4 has stream durations only until find_stream_info, which is too slow here.
        if (item.duration == 0.0 && st->duration != AV_NOPTS_VALUE && st->duration > 0 &&
            (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO))
            item.duration = std::max(item.duration, st->duration * av_q2d(st->time_base));
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) item.has_audio = true;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && item.width == 0) {
            item.width = par->width;
            item.height = par->height;
        }
    }
    avformat_close_input(&fmt);
}

}  // namespace

// --- media folder ------------------------------------------------------------------------

std::string default_media_dir()
{
    static std::string dir;
    static std::once_flag once;
    std::call_once(once, [] {
        std::string base = env_utf8(L"USERPROFILE");
        if (base.empty()) base = env_utf8(L"HOME");
        if (base.empty()) {
            wchar_t cwd[MAX_PATH * 4];
            DWORD n = GetCurrentDirectoryW(MAX_PATH * 4, cwd);
            base = narrow(std::wstring(cwd, n));
        }
        std::string videos = with_slash(base) + "Videos\\";
        dir = videos + "Glint";
        std::wstring glint = widen(dir), old = widen(videos + "Screenlink");
        if (!path_exists(glint) && path_exists(old)) {
            // Folder from before the rename. If it can't move (a file is open), keep using it this run.
            if (!MoveFileExW(old.c_str(), glint.c_str(), 0)) dir = videos + "Screenlink";
        }
    });
    return dir;
}

// --- screenshots ---------------------------------------------------------------------------

bool save_screenshot(const std::string& dir, const std::vector<uint8_t>& rgba, int width, int height,
                     std::string& out_path, std::string& error)
{
    if (width <= 0 || height <= 0 || rgba.size() < (size_t)width * height * 4) {
        error = "There is no picture to save.";
        return false;
    }
    if (!is_absolute(dir)) {
        error = "The screenshot folder must be a full path.";
        return false;
    }
    if (!make_dirs(widen(dir))) {
        error = "The screenshot folder could not be created.";
        return false;
    }
    std::vector<uint8_t> png;
    if (!encode_png(rgba.data(), width, height, png)) {
        error = "The screenshot could not be encoded.";
        return false;
    }
    std::string stem = with_slash(dir) + "Glint-Screenshot-" + local_stamp();
    for (int n = 1; n < 1000; n++) {
        std::string path = n == 1 ? stem + ".png" : stem + "-" + std::to_string(n) + ".png";
        std::wstring wpath = widen(path);
        HANDLE f = CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS) continue;
            error = "The screenshot file could not be created.";
            return false;
        }
        DWORD wrote = 0;
        bool ok = WriteFile(f, png.data(), (DWORD)png.size(), &wrote, nullptr) && wrote == png.size();
        ok = CloseHandle(f) && ok;
        if (!ok) {
            DeleteFileW(wpath.c_str());
            error = "Writing the screenshot failed.";
            return false;
        }
        out_path = path;
        return true;
    }
    error = "Too many screenshots with the same name.";
    return false;
}

// --- first picture of a file -------------------------------------------------------------

bool load_image_rgba(const std::string& path, std::vector<uint8_t>& rgba, int& width, int& height,
                     std::string& error)
{
    ffmpeg_setup();
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        error = "The file could not be opened.";
        return false;
    }
    avformat_find_stream_info(fmt, nullptr);
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const AVCodec* codec = vi >= 0 ? avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id) : nullptr;
    if (!codec) {
        avformat_close_input(&fmt);
        error = "The file has no picture that can be shown.";
        return false;
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool ok = false;
    if (ctx && pkt && frame && avcodec_parameters_to_context(ctx, fmt->streams[vi]->codecpar) >= 0) {
        ctx->thread_count = 1;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        if (avcodec_open2(ctx, codec, nullptr) >= 0) {
            bool eof = false;
            while (!ok) {
                if (!eof) {
                    int rc = av_read_frame(fmt, pkt);
                    if (rc < 0) {
                        eof = true;
                        avcodec_send_packet(ctx, nullptr);
                    } else {
                        if (pkt->stream_index == vi) avcodec_send_packet(ctx, pkt);
                        av_packet_unref(pkt);
                    }
                }
                int rc = avcodec_receive_frame(ctx, frame);
                if (rc == 0) {
                    ok = frame_to_rgba(frame, rgba, nullptr);
                    if (ok) {
                        width = frame->width;
                        height = frame->height;
                    }
                    av_frame_unref(frame);
                    if (!ok) break;
                } else if (rc != AVERROR(EAGAIN) || eof) {
                    if (eof) break;
                }
            }
        }
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    if (!ok) error = "No picture could be decoded from the file.";
    return ok;
}

// --- gallery -------------------------------------------------------------------------------

std::vector<GalleryItem> list_gallery(const std::string& dir)
{
    std::vector<GalleryItem> items;
    if (dir.empty()) return items;
    std::string folder = with_slash(dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(folder + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return items;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) continue;
        std::string name = narrow(fd.cFileName);
        std::string ext = lower_ext(name);
        if (ext != "mkv" && ext != "mp4" && ext != "png") continue;
        GalleryItem item;
        item.name = name;
        item.path = folder + name;
        item.video = ext != "png";
        item.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        uint64_t ft = ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        item.modified = ft > 116444736000000000ull ? (int64_t)((ft - 116444736000000000ull) / 10000000ull) : 0;
        items.push_back(item);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (GalleryItem& item : items) {
        if (item.video) probe_video(item.path, item);
        else png_size(widen(item.path), item.width, item.height);
    }
    std::sort(items.begin(), items.end(), [](const GalleryItem& a, const GalleryItem& b) {
        if (a.modified != b.modified) return a.modified > b.modified;
        return a.name > b.name;
    });
    return items;
}

struct FolderWatch::Impl {
    std::string dir;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool pending = false;
    ULONGLONG last_try = 0;

    void close()
    {
        if (handle != INVALID_HANDLE_VALUE) FindCloseChangeNotification(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    void open()
    {
        handle = FindFirstChangeNotificationW(
            widen(dir).c_str(), FALSE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
        if (handle != INVALID_HANDLE_VALUE) pending = true;   // the folder (re)appeared: refresh once
        last_try = GetTickCount64();
    }
};

FolderWatch::FolderWatch() : impl_(new Impl) {}

FolderWatch::~FolderWatch()
{
    impl_->close();
}

void FolderWatch::watch(const std::string& dir)
{
    Impl& d = *impl_;
    if (dir == d.dir) {
        // A missing folder is retried at most twice a second.
        if (d.handle == INVALID_HANDLE_VALUE && !dir.empty() && GetTickCount64() - d.last_try > 500) d.open();
        return;
    }
    d.close();
    d.dir = dir;
    d.pending = false;
    if (!dir.empty()) d.open();
}

bool FolderWatch::changed()
{
    Impl& d = *impl_;
    bool any = d.pending;
    d.pending = false;
    if (d.handle == INVALID_HANDLE_VALUE) return any;
    while (WaitForSingleObject(d.handle, 0) == WAIT_OBJECT_0) {
        any = true;
        if (!FindNextChangeNotification(d.handle)) {
            d.close();   // the folder went away; watch() picks it up again
            break;
        }
    }
    return any;
}

bool delete_gallery_file(const std::string& path, std::string& error)
{
    std::string ext = lower_ext(path);
    if (ext != "mkv" && ext != "mp4" && ext != "png") {
        error = "Only recordings and screenshots can be deleted here.";
        return false;
    }
    if (!DeleteFileW(widen(path).c_str())) {
        error = GetLastError() == ERROR_SHARING_VIOLATION ? "The file is in use and could not be deleted."
                                                          : "The file could not be deleted.";
        return false;
    }
    return true;
}

// --- export ------------------------------------------------------------------------------

bool export_copy(const std::string& source, const std::string& ext, std::string& out_path, std::string& error)
{
    if (ext != "mp4" && ext != "mkv") {
        error = "Export works only to MP4 or MKV.";
        return false;
    }
    ffmpeg_setup();
    size_t slash = source.find_last_of("\\/");
    std::string folder = slash == std::string::npos ? std::string() : source.substr(0, slash + 1);
    std::string name = slash == std::string::npos ? source : source.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    std::string out = folder + stem + "-export-" + local_stamp() + "." + ext;
    std::wstring wout = widen(out);
    if (_wcsicmp(wout.c_str(), widen(source).c_str()) == 0 || path_exists(wout)) {
        error = "An export with this name already exists; try again in a second.";
        return false;
    }

    AVFormatContext* in = nullptr;
    if (avformat_open_input(&in, source.c_str(), nullptr, nullptr) < 0) {
        error = "The recording could not be opened.";
        return false;
    }
    avformat_find_stream_info(in, nullptr);

    AVFormatContext* outc = nullptr;
    std::vector<int> map(in->nb_streams, -1);
    bool created = false;
    auto fail = [&](const std::string& why) {
        if (outc) {
            if (outc->pb) avio_closep(&outc->pb);
            avformat_free_context(outc);
        }
        avformat_close_input(&in);
        if (created) DeleteFileW(wout.c_str());
        error = why;
        return false;
    };

    if (avformat_alloc_output_context2(&outc, nullptr, ext == "mp4" ? "mp4" : "matroska", out.c_str()) < 0 || !outc) {
        outc = nullptr;
        return fail("The export could not be prepared.");
    }
    int streams = 0;
    for (unsigned i = 0; i < in->nb_streams; i++) {
        AVStream* is = in->streams[i];
        AVMediaType type = is->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO) continue;
        AVStream* os = avformat_new_stream(outc, nullptr);
        if (!os || avcodec_parameters_copy(os->codecpar, is->codecpar) < 0) return fail("Out of memory while exporting.");
        os->codecpar->codec_tag = 0;
        os->time_base = is->time_base;
        os->avg_frame_rate = is->avg_frame_rate;
        os->r_frame_rate = is->r_frame_rate;
        os->disposition = is->disposition;
        map[i] = os->index;
        streams++;
    }
    if (streams == 0) return fail("The file has no video or sound to export.");

    int rc = avio_open(&outc->pb, out.c_str(), AVIO_FLAG_WRITE);
    if (rc < 0) return fail("The export file could not be created (" + av_error(rc) + ").");
    created = true;
    AVDictionary* opts = nullptr;
    if (ext == "mp4") av_dict_set(&opts, "movflags", "+faststart", 0);
    rc = avformat_write_header(outc, &opts);
    av_dict_free(&opts);
    if (rc < 0) return fail("The export could not be written (" + av_error(rc) + ").");

    AVPacket* pkt = av_packet_alloc();
    std::vector<int64_t> last_dts(in->nb_streams, AV_NOPTS_VALUE);
    while (pkt && (rc = av_read_frame(in, pkt)) >= 0) {
        int oi = map[pkt->stream_index];
        if (oi < 0) {
            av_packet_unref(pkt);
            continue;
        }
        if (pkt->dts == AV_NOPTS_VALUE) pkt->dts = pkt->pts;
        int64_t& last = last_dts[pkt->stream_index];
        if (last != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->dts <= last) {
            // Keep decode times strictly increasing for MP4.
            pkt->dts = last + 1;
            if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) pkt->pts = pkt->dts;
        }
        last = pkt->dts;
        av_packet_rescale_ts(pkt, in->streams[pkt->stream_index]->time_base, outc->streams[oi]->time_base);
        pkt->stream_index = oi;
        pkt->pos = -1;
        rc = av_interleaved_write_frame(outc, pkt);
        if (rc < 0) {
            av_packet_free(&pkt);
            return fail("Writing the export failed (" + av_error(rc) + ").");
        }
    }
    av_packet_free(&pkt);
    rc = av_write_trailer(outc);
    if (rc < 0) return fail("Finishing the export failed (" + av_error(rc) + ").");
    avio_closep(&outc->pb);
    avformat_free_context(outc);
    avformat_close_input(&in);
    out_path = out;
    logf("export: %s -> %s", source.c_str(), out.c_str());
    return true;
}

bool open_with_default_app(const std::string& path)
{
    INT_PTR rc = (INT_PTR)ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return rc > 32;
}

}
