#include "settings.h"
#include "winutil.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>

namespace glint {

bool PictureSettings::operator==(const PictureSettings& o) const
{
    return profile == o.profile && protect == o.protect && budget_ms == o.budget_ms && upscaler == o.upscaler &&
           sharpness == o.sharpness && denoise == o.denoise && deband == o.deband && dither == o.dither &&
           exposure == o.exposure && saturation == o.saturation && shadows == o.shadows;
}

bool PicturePreset::operator==(const PicturePreset& o) const
{
    return name == o.name && values == o.values;
}

bool AppSettings::operator==(const AppSettings& o) const
{
    return console_url == o.console_url && recent == o.recent && sound == o.sound && volume == o.volume &&
           delay_auto == o.delay_auto && delay_ms == o.delay_ms && video_sync == o.video_sync &&
           av_offset_ms == o.av_offset_ms && media_dir == o.media_dir && gpu_decode == o.gpu_decode &&
           gpu_convert == o.gpu_convert && latency == o.latency && enhance == o.enhance &&
           picture_presets == o.picture_presets && picture_active == o.picture_active && picture == o.picture &&
           window_x == o.window_x && window_y == o.window_y && window_w == o.window_w && window_h == o.window_h &&
           maximized == o.maximized && fullscreen == o.fullscreen && theme == o.theme;
}

bool AppSettings::operator!=(const AppSettings& o) const
{
    return !(*this == o);
}

namespace {

bool dir_exists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// CreateDirectoryW for each missing level.
void make_dirs(const std::wstring& path)
{
    if (path.empty() || dir_exists(path)) return;
    size_t cut = path.find_last_of(L"\\/");
    if (cut != std::wstring::npos && cut > 0) make_dirs(path.substr(0, cut));
    CreateDirectoryW(path.c_str(), nullptr);
}

// First run after the rename: bring the old *.conf files over.
void migrate_from_screenlink(const std::wstring& base, const std::wstring& target)
{
    std::wstring old = base + L"\\Screenlink";
    if (dir_exists(target) || !dir_exists(old)) return;
    make_dirs(target);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((old + L"\\*.conf").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        CopyFileW((old + L"\\" + fd.cFileName).c_str(), (target + L"\\" + fd.cFileName).c_str(), TRUE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool has_line_break(const std::string& s)
{
    return s.find_first_of("\r\n") != std::string::npos;
}

bool starts_with(const std::string& s, const char* prefix)
{
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

bool is_web_url(const std::string& s)
{
    return starts_with(s, "http://") || starts_with(s, "https://");
}

bool is_absolute_dir(const std::string& s)
{
    if (s.size() >= 3 && std::isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/'))
        return true;
    return s.size() >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/');
}

bool parse_int(const std::string& v, long& out)
{
    if (v.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long x = std::strtol(v.c_str(), &end, 10);
    if (errno || !end || *end) return false;
    out = x;
    return true;
}

bool parse_float(const std::string& v, float& out)
{
    if (v.empty()) return false;
    char* end = nullptr;
    double x = std::strtod(v.c_str(), &end);
    if (!end || *end || !std::isfinite(x)) return false;
    out = (float)x;
    return true;
}

std::string fmt_float(float f)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", (double)f);
    return buf;
}

// One parsed settings file.
struct ConfFile {
    std::string name;
    std::map<std::string, std::string> values;
    std::vector<std::string>* warnings = nullptr;

    bool has(const std::string& key) const { return values.count(key) != 0; }
    const std::string& get(const std::string& key) const { return values.at(key); }

    void warn(const std::string& key, const char* what)
    {
        warnings->push_back(name + ": " + key + " " + what + "; using the default");
    }

    void read_bool(const char* key, bool& field)
    {
        if (!has(key)) return;
        const std::string& v = get(key);
        if (v == "0") field = false;
        else if (v == "1") field = true;
        else warn(key, "is not 0 or 1");
    }

    void read_int(const char* key, int& field, long lo, long hi)
    {
        if (!has(key)) return;
        long x;
        if (!parse_int(get(key), x)) warn(key, "is not a whole number");
        else if (x < lo || x > hi) warn(key, "is out of range");
        else field = (int)x;
    }

    void read_float(const char* key, float& field, float lo, float hi)
    {
        if (!has(key)) return;
        float x;
        if (!parse_float(get(key), x)) warn(key, "is not a number");
        else if (x < lo || x > hi) warn(key, "is out of range");
        else field = x;
    }

    // A word from a fixed list; anything else silently keeps the default.
    void read_choice(const char* key, int& field, std::initializer_list<const char*> words)
    {
        if (!has(key)) return;
        int i = 0;
        for (const char* w : words) {
            if (get(key) == w) {
                field = i;
                return;
            }
            i++;
        }
    }
};

// False when the file is missing or has the wrong header (one warning then).
bool load_conf(const std::string& dir, const char* name, const char* header, ConfFile& conf,
               std::vector<std::string>& warnings)
{
    conf.name = name;
    conf.warnings = &warnings;
    FILE* f = _wfopen(widen(dir + "\\" + name).c_str(), L"rb");
    if (!f) return false;
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        lines.push_back(text.substr(pos, nl - pos));
        pos = nl + 1;
    }
    std::string first = lines.empty() ? std::string() : trim(lines[0]);
    std::string old_header = std::string("SCREENLINK-") + (header + 6);   // "GLINT-" -> "SCREENLINK-"
    if (first != header && first != old_header) {
        warnings.push_back(std::string(name) + ": not a Glint settings file; ignored");
        return false;
    }
    for (size_t i = 1; i < lines.size(); i++) {
        std::string line = trim(lines[i]);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        conf.values[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return true;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        size_t at = s.find(sep, pos);
        out.push_back(s.substr(pos, at == std::string::npos ? std::string::npos : at - pos));
        if (at == std::string::npos) break;
        pos = at + 1;
    }
    return out;
}

bool parse_preset(const std::string& line, PicturePreset& preset)
{
    std::vector<std::string> f = split(line, '|');
    if (f.size() != 9 || f[0].empty()) return false;
    preset = PicturePreset();
    preset.name = f[0];
    PictureSettings& p = preset.values;
    p.profile = 4;
    long i;
    if (parse_int(trim(f[1]), i) && i >= 0 && i <= 3) p.upscaler = (int)i;
    auto num = [&](const std::string& s, float& field, float lo, float hi) {
        float x;
        if (parse_float(trim(s), x) && x >= lo && x <= hi) field = x;
    };
    num(f[2], p.sharpness, 0.0f, 1.0f);
    num(f[3], p.denoise, 0.0f, 1.0f);
    num(f[4], p.deband, 0.0f, 1.0f);
    num(f[5], p.dither, 0.0f, 1.0f);
    num(f[6], p.exposure, -2.0f, 2.0f);
    num(f[7], p.saturation, 0.0f, 2.0f);
    num(f[8], p.shadows, 0.0f, 1.0f);
    return true;
}

bool write_atomic(const std::string& dir, const char* name, const std::string& text, std::string& error)
{
    std::wstring path = widen(dir + "\\" + name);
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    bool ok = f != nullptr;
    if (ok) {
        ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = std::fflush(f) == 0 && ok;
        ok = std::fclose(f) == 0 && ok;
    }
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) {
        DeleteFileW(tmp.c_str());
        error = std::string("Could not save ") + name + ".";
    }
    return ok;
}

std::string strip_chars(const std::string& s, const char* chars)
{
    std::string out;
    for (char c : s)
        if (!std::strchr(chars, c)) out += c;
    return out;
}

}  // namespace

std::string settings_dir()
{
    std::string custom = env_utf8(L"GLINT_SETTINGS_DIR");
    if (!custom.empty()) {
        make_dirs(widen(custom));
        return custom;
    }
    std::wstring base = widen(env_utf8(L"LOCALAPPDATA"));
    if (base.empty()) {
        wchar_t cwd[MAX_PATH * 4];
        DWORD n = GetCurrentDirectoryW(MAX_PATH * 4, cwd);
        return narrow(std::wstring(cwd, n));
    }
    std::wstring dir = base + L"\\Glint";
    static std::once_flag once;
    std::call_once(once, [&] { migrate_from_screenlink(base, dir); });
    make_dirs(dir);
    return narrow(dir);
}

void load_settings(AppSettings& s, std::vector<std::string>& warnings)
{
    s = AppSettings();
    std::string dir = settings_dir();
    ConfFile c;

    if (load_conf(dir, "console.conf", "GLINT-CONSOLE-1", c, warnings)) {
        if (c.has("address")) {
            const std::string& v = c.get("address");
            if (v.empty() || is_web_url(v)) s.console_url = v;
            else c.warn("address", "is not an http:// address");
        }
        for (int i = 1; i <= 4; i++) {
            std::string key = "recent" + std::to_string(i);
            if (c.has(key) && is_web_url(c.get(key))) s.recent.push_back(c.get(key));
        }
    }

    c = ConfFile();
    if (load_conf(dir, "audio.conf", "GLINT-AUDIO-1", c, warnings)) {
        c.read_bool("sound", s.sound);
        c.read_float("volume", s.volume, 0.0f, 4.0f);
        c.read_bool("delay_auto", s.delay_auto);
        c.read_int("delay_ms", s.delay_ms, 0, 400);
        c.read_bool("video_sync", s.video_sync);
        c.read_int("av_offset_ms", s.av_offset_ms, -200, 200);
    }

    c = ConfFile();
    if (load_conf(dir, "video.conf", "GLINT-VIDEO-1", c, warnings)) {
        if (c.has("decoder")) {
            if (c.get("decoder") == "gpu") s.gpu_decode = true;
            else if (c.get("decoder") == "cpu") s.gpu_decode = false;
        }
        if (c.has("conversion")) {
            if (c.get("conversion") == "gpu") s.gpu_convert = true;
            else if (c.get("conversion") == "cpu") s.gpu_convert = false;
        }
        c.read_choice("latency", s.latency, {"lowest", "balanced", "smooth"});
    }

    c = ConfFile();
    if (load_conf(dir, "picture.conf", "GLINT-PICTURE-1", c, warnings)) {
        PictureSettings& p = s.enhance;
        c.read_int("profile", p.profile, 0, 4);
        c.read_bool("protect", p.protect);
        c.read_float("budget_ms", p.budget_ms, 0.5f, 10.0f);
        c.read_int("upscaler", p.upscaler, 0, 3);
        c.read_float("sharpness", p.sharpness, 0.0f, 1.0f);
        c.read_float("denoise", p.denoise, 0.0f, 1.0f);
        c.read_float("deband", p.deband, 0.0f, 1.0f);
        c.read_float("dither", p.dither, 0.0f, 1.0f);
        c.read_float("shadows", p.shadows, 0.0f, 1.0f);
        c.read_float("exposure", p.exposure, -2.0f, 2.0f);
        c.read_float("saturation", p.saturation, 0.0f, 2.0f);
        if (c.has("active")) s.picture_active = c.get("active");
        for (int i = 1; i <= kMaxPicturePresets; i++) {
            std::string key = "preset" + std::to_string(i);
            if (!c.has(key)) continue;
            PicturePreset preset;
            if (parse_preset(c.get(key), preset)) s.picture_presets.push_back(preset);
            else warnings.push_back("picture.conf: " + key + " is not a valid preset; skipped");
        }
    }

    c = ConfFile();
    if (load_conf(dir, "capture.conf", "GLINT-CAPTURE-1", c, warnings)) {
        if (c.has("media_dir")) {
            const std::string& v = c.get("media_dir");
            if (v.empty() || is_absolute_dir(v)) s.media_dir = v;
            else c.warn("media_dir", "is not an absolute folder");
        }
    }

    c = ConfFile();
    if (load_conf(dir, "window.conf", "GLINT-WINDOW-1", c, warnings)) {
        c.read_choice("picture", s.picture, {"fit", "fill", "native"});
        c.read_int("x", s.window_x, -32000, 32000);
        c.read_int("y", s.window_y, -32000, 32000);
        c.read_int("width", s.window_w, 320, 16384);
        c.read_int("height", s.window_h, 240, 16384);
        c.read_bool("maximized", s.maximized);
        c.read_bool("fullscreen", s.fullscreen);
        c.read_choice("theme", s.theme, {"dark", "light"});
    }
}

bool save_settings(const AppSettings& s, std::string& error)
{
    bool broken = has_line_break(s.console_url) || has_line_break(s.media_dir);
    for (const auto& r : s.recent) broken = broken || has_line_break(r);
    if (broken) {
        error = "A setting contains a line break; settings were not saved.";
        return false;
    }
    auto b = [](bool v) { return v ? "1" : "0"; };
    std::string dir = settings_dir();

    std::string console = "GLINT-CONSOLE-1\naddress=" + s.console_url + "\n";
    for (size_t i = 0; i < s.recent.size() && i < 4; i++)
        console += "recent" + std::to_string(i + 1) + "=" + s.recent[i] + "\n";

    std::string audio = "GLINT-AUDIO-1\n";
    audio += std::string("sound=") + b(s.sound) + "\n";
    audio += "volume=" + fmt_float(s.volume) + "\n";
    audio += std::string("delay_auto=") + b(s.delay_auto) + "\n";
    audio += "delay_ms=" + std::to_string(s.delay_ms) + "\n";
    audio += std::string("video_sync=") + b(s.video_sync) + "\n";
    audio += "av_offset_ms=" + std::to_string(s.av_offset_ms) + "\n";

    static const char* kLatency[] = {"lowest", "balanced", "smooth"};
    std::string video = "GLINT-VIDEO-1\n";
    video += std::string("decoder=") + (s.gpu_decode ? "gpu" : "cpu") + "\n";
    video += std::string("conversion=") + (s.gpu_convert ? "gpu" : "cpu") + "\n";
    video += std::string("latency=") + kLatency[s.latency >= 0 && s.latency <= 2 ? s.latency : 0] + "\n";

    const PictureSettings& p = s.enhance;
    std::string picture = "GLINT-PICTURE-1\n";
    picture += "profile=" + std::to_string(p.profile) + "\n";
    picture += std::string("protect=") + b(p.protect) + "\n";
    picture += "budget_ms=" + fmt_float(p.budget_ms) + "\n";
    picture += "upscaler=" + std::to_string(p.upscaler) + "\n";
    picture += "sharpness=" + fmt_float(p.sharpness) + "\n";
    picture += "denoise=" + fmt_float(p.denoise) + "\n";
    picture += "deband=" + fmt_float(p.deband) + "\n";
    picture += "dither=" + fmt_float(p.dither) + "\n";
    picture += "exposure=" + fmt_float(p.exposure) + "\n";
    picture += "saturation=" + fmt_float(p.saturation) + "\n";
    picture += "shadows=" + fmt_float(p.shadows) + "\n";
    std::string active = strip_chars(s.picture_active, "\r\n");
    if (!active.empty()) picture += "active=" + active + "\n";
    for (size_t i = 0; i < s.picture_presets.size() && i < (size_t)kMaxPicturePresets; i++) {
        const PicturePreset& pr = s.picture_presets[i];
        const PictureSettings& v = pr.values;
        picture += "preset" + std::to_string(i + 1) + "=" + strip_chars(pr.name, "|\r\n") + "|" +
                   std::to_string(v.upscaler) + "|" + fmt_float(v.sharpness) + "|" + fmt_float(v.denoise) + "|" +
                   fmt_float(v.deband) + "|" + fmt_float(v.dither) + "|" + fmt_float(v.exposure) + "|" +
                   fmt_float(v.saturation) + "|" + fmt_float(v.shadows) + "\n";
    }

    std::string capture = "GLINT-CAPTURE-1\nmedia_dir=" + s.media_dir + "\n";

    static const char* kPicture[] = {"fit", "fill", "native"};
    std::string window = "GLINT-WINDOW-1\n";
    window += std::string("picture=") + kPicture[s.picture >= 0 && s.picture <= 2 ? s.picture : 0] + "\n";
    if (s.window_w > 0 && s.window_h > 0) {
        window += "x=" + std::to_string(s.window_x) + "\n";
        window += "y=" + std::to_string(s.window_y) + "\n";
        window += "width=" + std::to_string(s.window_w) + "\n";
        window += "height=" + std::to_string(s.window_h) + "\n";
    }
    window += std::string("maximized=") + b(s.maximized) + "\n";
    window += std::string("fullscreen=") + b(s.fullscreen) + "\n";
    window += std::string("theme=") + (s.theme == 1 ? "light" : "dark") + "\n";

    return write_atomic(dir, "console.conf", console, error) && write_atomic(dir, "audio.conf", audio, error) &&
           write_atomic(dir, "video.conf", video, error) && write_atomic(dir, "picture.conf", picture, error) &&
           write_atomic(dir, "capture.conf", capture, error) && write_atomic(dir, "window.conf", window, error);
}

}
