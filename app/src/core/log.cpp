#include "log.h"
#include "settings.h"
#include "winutil.h"

#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <zlib.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <vector>

namespace glint {

namespace {

constexpr long long kLogCap = 8ll * 1024 * 1024;

std::mutex g_log_mutex;
FILE* g_log = nullptr;
std::string g_log_path;
std::string g_prev_path;
long long g_log_bytes = 0;
bool g_log_capped = false;

std::string join_path(const std::string& dir, const char* name)
{
    if (dir.empty()) return name;
    char last = dir.back();
    return dir + (last == '\\' || last == '/' ? "" : "\\") + name;
}

std::string stamp_now(const char* fmt)
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), fmt, &tm);
    return buf;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out)
{
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// Minimal zip writer: deflated entries, UTF-8 names.
class ZipWriter {
public:
    explicit ZipWriter(FILE* f) : f_(f) {}

    bool add(const std::string& name, const std::vector<uint8_t>& data)
    {
        uLong crc = crc32(0L, Z_NULL, 0);
        if (!data.empty()) crc = crc32(crc, data.data(), (uInt)data.size());
        std::vector<uint8_t> packed;
        uint16_t method = 8;
        if (!deflate_raw(data, packed)) return false;
        if (packed.size() >= data.size()) {   // incompressible: store
            packed = data;
            method = 0;
        }
        Entry e;
        e.name = name;
        e.crc = (uint32_t)crc;
        e.csize = (uint32_t)packed.size();
        e.usize = (uint32_t)data.size();
        e.method = method;
        e.offset = (uint32_t)offset_;
        dos_time(e.time, e.date);

        std::vector<uint8_t> h;
        put32(h, 0x04034b50);
        put16(h, 20);
        put16(h, 0x0800);
        put16(h, method);
        put16(h, e.time);
        put16(h, e.date);
        put32(h, e.crc);
        put32(h, e.csize);
        put32(h, e.usize);
        put16(h, (uint16_t)name.size());
        put16(h, 0);
        h.insert(h.end(), name.begin(), name.end());
        if (!write(h) || !write(packed)) return false;
        entries_.push_back(e);
        return true;
    }

    bool finish()
    {
        size_t cd_start = offset_;
        std::vector<uint8_t> cd;
        for (const Entry& e : entries_) {
            put32(cd, 0x02014b50);
            put16(cd, 20);
            put16(cd, 20);
            put16(cd, 0x0800);
            put16(cd, e.method);
            put16(cd, e.time);
            put16(cd, e.date);
            put32(cd, e.crc);
            put32(cd, e.csize);
            put32(cd, e.usize);
            put16(cd, (uint16_t)e.name.size());
            put16(cd, 0);
            put16(cd, 0);
            put16(cd, 0);
            put16(cd, 0);
            put32(cd, 0);
            put32(cd, e.offset);
            cd.insert(cd.end(), e.name.begin(), e.name.end());
        }
        std::vector<uint8_t> end;
        put32(end, 0x06054b50);
        put16(end, 0);
        put16(end, 0);
        put16(end, (uint16_t)entries_.size());
        put16(end, (uint16_t)entries_.size());
        put32(end, (uint32_t)cd.size());
        put32(end, (uint32_t)cd_start);
        put16(end, 0);
        return write(cd) && write(end);
    }

private:
    struct Entry {
        std::string name;
        uint32_t crc = 0, csize = 0, usize = 0, offset = 0;
        uint16_t method = 0, time = 0, date = 0;
    };

    static void put16(std::vector<uint8_t>& v, uint16_t x)
    {
        v.push_back((uint8_t)x);
        v.push_back((uint8_t)(x >> 8));
    }
    static void put32(std::vector<uint8_t>& v, uint32_t x)
    {
        for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
    }
    static void dos_time(uint16_t& t, uint16_t& d)
    {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        t = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
        d = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    }
    static bool deflate_raw(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
    {
        z_stream z{};
        if (deflateInit2(&z, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
        out.resize(deflateBound(&z, (uLong)in.size()) + 16);
        z.next_in = const_cast<Bytef*>(in.data());
        z.avail_in = (uInt)in.size();
        z.next_out = out.data();
        z.avail_out = (uInt)out.size();
        int rc = deflate(&z, Z_FINISH);
        out.resize(z.total_out);
        deflateEnd(&z);
        return rc == Z_STREAM_END;
    }
    bool write(const std::vector<uint8_t>& v)
    {
        if (v.empty()) return true;
        if (std::fwrite(v.data(), 1, v.size(), f_) != v.size()) return false;
        offset_ += v.size();
        return true;
    }

    FILE* f_;
    size_t offset_ = 0;
    std::vector<Entry> entries_;
};

}  // namespace

void log_open(const std::string& dir)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log) return;
    std::string path = join_path(dir, "glint.log");
    std::string prev = join_path(dir, "glint.previous.log");
    std::wstring wpath = widen(path);
    if (GetFileAttributesW(wpath.c_str()) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(wpath.c_str(), widen(prev).c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = _wfopen(wpath.c_str(), L"wb");
    if (!g_log) return;
    g_log_path = path;
    g_prev_path = prev;
    g_log_bytes = 0;
    g_log_capped = false;
}

void log_close()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
}

void logf(const char* fmt, ...)
{
    char text[4096];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[4200];
    int n = std::snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", st.wYear, st.wMonth,
                          st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_log || g_log_capped) return;
    if (g_log_bytes + n > kLogCap) {
        static const char kCapped[] = "log size limit reached; further lines are dropped\n";
        std::fwrite(kCapped, 1, sizeof(kCapped) - 1, g_log);
        std::fflush(g_log);
        g_log_capped = true;
        return;
    }
    std::fwrite(line, 1, (size_t)n, g_log);
    std::fflush(g_log);
    g_log_bytes += n;
}

std::string log_path()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_log ? g_log_path : std::string();
}

std::string previous_log_path()
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_prev_path;
}

bool create_diagnostics(const std::string& out_dir, const std::string& system_info,
                        std::string& out_path, std::string& error)
{
    std::string sdir = settings_dir();
    std::string current, previous;
    {
        std::lock_guard<std::mutex> lock(g_log_mutex);
        if (g_log) std::fflush(g_log);
        current = g_log ? g_log_path : join_path(sdir, "glint.log");
        previous = g_prev_path.empty() ? join_path(sdir, "glint.previous.log") : g_prev_path;
    }

    std::string path = join_path(out_dir, ("Glint-diagnostics-" + stamp_now("%Y%m%d-%H%M%S") + ".zip").c_str());
    std::wstring wpath = widen(path);
    FILE* f = _wfopen(wpath.c_str(), L"wb");
    if (!f) {
        error = "Could not create " + path + ".";
        return false;
    }
    ZipWriter zip(f);
    bool ok = zip.add("system.txt", std::vector<uint8_t>(system_info.begin(), system_info.end()));
    std::vector<uint8_t> data;
    if (ok && read_file(current, data)) ok = zip.add("glint.log", data);
    if (ok && read_file(previous, data)) ok = zip.add("glint.previous.log", data);
    static const char* kConfs[] = {"console.conf", "audio.conf", "video.conf",
                                   "picture.conf", "capture.conf", "window.conf"};
    for (const char* name : kConfs) {
        if (ok && read_file(join_path(sdir, name), data)) ok = zip.add(std::string("config/") + name, data);
    }
    if (ok) ok = zip.finish();
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        DeleteFileW(wpath.c_str());
        error = "Could not write the diagnostics file.";
        return false;
    }
    out_path = path;
    return true;
}

std::string downloads_dir()
{
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) out = narrow(p);
    if (p) CoTaskMemFree(p);
    return out.empty() ? settings_dir() : out;
}

std::string windows_version()
{
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? (RtlGetVersionFn)(void*)GetProcAddress(ntdll, "RtlGetVersion") : nullptr;
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (!fn || fn(&info) != 0) return "Windows (unknown version)";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Windows %lu.%lu.%lu", (unsigned long)info.dwMajorVersion,
                  (unsigned long)info.dwMinorVersion, (unsigned long)info.dwBuildNumber);
    return buf;
}

void reveal_in_explorer(const std::string& path)
{
    std::wstring args = L"/select,\"" + widen(path) + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

double process_cpu_seconds()
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    auto secs = [](const FILETIME& ft) {
        ULARGE_INTEGER v;
        v.LowPart = ft.dwLowDateTime;
        v.HighPart = ft.dwHighDateTime;
        return (double)v.QuadPart / 1e7;
    };
    return secs(kernel) + secs(user);
}

void attach_parent_console()
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD type = GetFileType(out);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) return;   // already redirected
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;   // double-clicked
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
}

}
