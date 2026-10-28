#include "log.h"

#include "settings.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <zlib.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

namespace glint {
namespace {

std::mutex g_mutex;
std::FILE* g_file = nullptr;
std::string g_path;
std::string g_previous;
size_t g_written = 0;
constexpr size_t kLogCap = 8u << 20;

void put_u16(std::vector<unsigned char>& b, unsigned v) {
    b.push_back((unsigned char)(v & 0xff));
    b.push_back((unsigned char)((v >> 8) & 0xff));
}
void put_u32(std::vector<unsigned char>& b, unsigned long v) {
    for (int i = 0; i < 4; i++) b.push_back((unsigned char)((v >> (8 * i)) & 0xff));
}

} // namespace

void log_open(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    fs::path d = fs::u8path(dir);
    g_path = (d / "glint.log").u8string();
    g_previous = (d / "glint.previous.log").u8string();
    std::error_code ec;
    if (fs::exists(fs::u8path(g_path), ec)) {
        fs::remove(fs::u8path(g_previous), ec);
        fs::rename(fs::u8path(g_path), fs::u8path(g_previous), ec);
    }
    g_file = _wfopen(fs::u8path(g_path).wstring().c_str(), L"wb");
    g_written = 0;
}

void log_close() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    g_file = nullptr;
}

void logf(const char* fmt, ...) {
    char text[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm local{};
    localtime_s(&local, &t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file) return;
    if (g_written > kLogCap) return;
    int n = std::fprintf(g_file, "%s.%03d %s\n", stamp, ms, text);
    if (n > 0) g_written += (size_t)n;
    if (g_written > kLogCap) std::fprintf(g_file, "%s.%03d log size limit reached; further lines dropped\n", stamp, ms);
    std::fflush(g_file);
}

std::string log_path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path;
}

std::string previous_log_path() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_previous;
}

std::string windows_version() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    auto fn = (RtlGetVersionFn)(void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {};
    v.dwOSVersionInfoSize = sizeof(v);
    if (!fn || fn(&v) != 0) return "Windows (unknown version)";
    char b[64];
    std::snprintf(b, sizeof(b), "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return b;
}

void reveal_in_explorer(const std::string& path) {
    std::wstring args = L"/select,\"" + fs::u8path(path).wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

double process_cpu_seconds() {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0.0;
    auto secs = [](const FILETIME& f) {
        return (double)(((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7;
    };
    return secs(kernel) + secs(user);
}

void attach_parent_console() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;   // file or pipe
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;   // double-clicked: there is no terminal
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
}

std::string downloads_dir() {
    PWSTR p = nullptr;
    std::string out;
    if (SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p) == S_OK && p) {
        out = fs::path(p).u8string();
    }
    if (p) CoTaskMemFree(p);
    return out.empty() ? settings_dir() : out;
}

// Minimal ZIP writer (stored entries, UTF-8 names): no compression library
// needed beyond zlib's crc32, and every zip tool opens it.
bool create_diagnostics(const std::string& out_dir, const std::string& system_info, std::string& out_path,
                        std::string& error) {
    struct Entry { std::string name; std::string data; };
    std::vector<Entry> entries;
    entries.push_back({"system.txt", system_info});
    auto add_file = [&](const std::string& path, const std::string& name) {
        std::ifstream in(fs::u8path(path), std::ios::binary);
        if (!in) return;
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        entries.push_back({name, data});
    };
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file) std::fflush(g_file);
    }
    add_file(log_path(), "glint.log");
    add_file(previous_log_path(), "glint.previous.log");
    for (const std::string& rel : shareable_settings_files())
        add_file((fs::u8path(settings_dir()) / fs::u8path(rel)).u8string(), "config/" + rel);

    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    char name[64];
    std::strftime(name, sizeof(name), "Glint-diagnostics-%Y%m%d-%H%M%S.zip", &local);
    unsigned dos_time = (unsigned)((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    unsigned dos_date = (unsigned)(((local.tm_year - 80) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);

    std::vector<unsigned char> zip, central;
    for (const Entry& e : entries) {
        unsigned long crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, (const Bytef*)e.data.data(), (uInt)e.data.size());
        unsigned long offset = (unsigned long)zip.size();
        put_u32(zip, 0x04034b50);
        put_u16(zip, 20);
        put_u16(zip, 0x0800);   // UTF-8 names
        put_u16(zip, 0);        // stored
        put_u16(zip, dos_time);
        put_u16(zip, dos_date);
        put_u32(zip, crc);
        put_u32(zip, (unsigned long)e.data.size());
        put_u32(zip, (unsigned long)e.data.size());
        put_u16(zip, (unsigned)e.name.size());
        put_u16(zip, 0);
        zip.insert(zip.end(), e.name.begin(), e.name.end());
        zip.insert(zip.end(), e.data.begin(), e.data.end());

        put_u32(central, 0x02014b50);
        put_u16(central, 20);
        put_u16(central, 20);
        put_u16(central, 0x0800);
        put_u16(central, 0);
        put_u16(central, dos_time);
        put_u16(central, dos_date);
        put_u32(central, crc);
        put_u32(central, (unsigned long)e.data.size());
        put_u32(central, (unsigned long)e.data.size());
        put_u16(central, (unsigned)e.name.size());
        put_u16(central, 0);
        put_u16(central, 0);
        put_u16(central, 0);
        put_u16(central, 0);
        put_u32(central, 0);
        put_u32(central, offset);
        central.insert(central.end(), e.name.begin(), e.name.end());
    }
    unsigned long central_offset = (unsigned long)zip.size();
    zip.insert(zip.end(), central.begin(), central.end());
    put_u32(zip, 0x06054b50);
    put_u16(zip, 0);
    put_u16(zip, 0);
    put_u16(zip, (unsigned)entries.size());
    put_u16(zip, (unsigned)entries.size());
    put_u32(zip, (unsigned long)central.size());
    put_u32(zip, central_offset);
    put_u16(zip, 0);

    fs::path out = fs::u8path(out_dir) / name;
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write((const char*)zip.data(), (std::streamsize)zip.size());
    f.close();
    if (!f) {
        error = "Could not write the diagnostics file.";
        return false;
    }
    out_path = out.u8string();
    return true;
}

} // namespace glint
