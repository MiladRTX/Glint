#include "settings.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace glint {
namespace {

std::string trim(const std::string& v) {
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

fs::path file_in(const char* name) {
    return fs::u8path(settings_dir()) / fs::u8path(name);
}

// key=value file with a fixed first line. Returns false when absent. Files
// saved before the rename start with SCREENLINK- instead of GLINT- and still load.
bool read_kv(const fs::path& path, const char* header, std::map<std::string, std::string>& out,
             std::vector<std::string>& warnings) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || !is_header(trim(line), header)) {
        warnings.push_back(path.filename().u8string() + ": unknown format, using defaults");
        return false;
    }
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return true;
}

struct Reader {
    std::map<std::string, std::string>& kv;
    std::vector<std::string>& warnings;
    std::string file;

    void boolean(const char* key, bool& v) {
        auto it = kv.find(key);
        if (it == kv.end()) return;
        if (it->second == "1" || it->second == "0") v = it->second == "1";
        else warnings.push_back(file + ": bad " + key);
    }
    void integer(const char* key, int& v, int lo, int hi) {
        auto it = kv.find(key);
        if (it == kv.end()) return;
        char* end = nullptr;
        long n = std::strtol(it->second.c_str(), &end, 10);
        if (end && *end == 0 && !it->second.empty() && n >= lo && n <= hi) v = (int)n;
        else warnings.push_back(file + ": bad " + key);
    }
    void real(const char* key, float& v, float lo, float hi) {
        auto it = kv.find(key);
        if (it == kv.end()) return;
        char* end = nullptr;
        double n = std::strtod(it->second.c_str(), &end);
        if (end && *end == 0 && !it->second.empty() && std::isfinite(n) && n >= lo && n <= hi) v = (float)n;
        else warnings.push_back(file + ": bad " + key);
    }
    void text(const char* key, std::string& v) {
        auto it = kv.find(key);
        if (it != kv.end()) v = it->second;
    }
};

bool one_line(const std::string& v) {
    return v.find_first_of("\r\n") == std::string::npos;
}

} // namespace

bool is_header(const std::string& line, const char* header) {
    if (line == header) return true;
    const std::string glint = "GLINT-";
    std::string h = header;
    return h.rfind(glint, 0) == 0 && line == "SCREENLINK-" + h.substr(glint.size());
}

std::string settings_dir() {
    const char* override_dir = std::getenv("GLINT_SETTINGS_DIR");
    fs::path dir;
    if (override_dir && *override_dir) {
        dir = fs::u8path(override_dir);
    } else {
        const char* local = std::getenv("LOCALAPPDATA");
        fs::path base = local ? fs::u8path(local) : fs::current_path();
        dir = base / "Glint";
        // First run after the rename from Screenlink: copy the old settings
        // over (the old folder is left as it was).
        std::error_code ec;
        if (!fs::exists(dir, ec) && fs::is_directory(base / "Screenlink", ec)) {
            fs::create_directories(dir, ec);
            for (const auto& e : fs::directory_iterator(base / "Screenlink", ec))
                if (e.path().extension() == ".conf")
                    fs::copy_file(e.path(), dir / e.path().filename(), fs::copy_options::skip_existing, ec);
        }
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.u8string();
}

bool write_file_atomic(const std::string& path_utf8, const std::string& text, std::string& error) {
    fs::path path = fs::u8path(path_utf8);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        if (!out) {
            error = "Cannot save " + path.filename().u8string();
            return false;
        }
    }
    if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(tmp, ec);
        error = "Cannot replace " + path.filename().u8string();
        return false;
    }
    return true;
}

bool AppSettings::operator==(const AppSettings& o) const {
    return console_url == o.console_url && recent == o.recent && theme == o.theme && sound == o.sound && volume == o.volume && delay_auto == o.delay_auto &&
           delay_ms == o.delay_ms && video_sync == o.video_sync && av_offset_ms == o.av_offset_ms &&
           media_dir == o.media_dir && picture == o.picture && gpu_decode == o.gpu_decode &&
           gpu_convert == o.gpu_convert && latency == o.latency && enhance == o.enhance &&
           picture_presets == o.picture_presets && picture_active == o.picture_active && window_x == o.window_x &&
           window_y == o.window_y && window_w == o.window_w && window_h == o.window_h &&
           maximized == o.maximized && fullscreen == o.fullscreen;
}

void load_settings(AppSettings& s, std::vector<std::string>& warnings) {
    s = AppSettings{};
    {
        std::map<std::string, std::string> kv;
        if (read_kv(file_in("console.conf"), "GLINT-CONSOLE-1", kv, warnings)) {
            Reader r{kv, warnings, "console.conf"};
            r.text("address", s.console_url);
            if (!s.console_url.empty() && s.console_url.rfind("http://", 0) != 0 && s.console_url.rfind("https://", 0) != 0) {
                warnings.push_back("console.conf: bad address");
                s.console_url.clear();
            }
            for (int i = 1; i <= 4; i++) {
                std::string url;
                r.text(("recent" + std::to_string(i)).c_str(), url);
                if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) s.recent.push_back(url);
            }
        }
    }
    {
        std::map<std::string, std::string> kv;
        if (read_kv(file_in("audio.conf"), "GLINT-AUDIO-1", kv, warnings)) {
            Reader r{kv, warnings, "audio.conf"};
            r.boolean("sound", s.sound);
            r.real("volume", s.volume, 0.0f, 4.0f);
            r.boolean("delay_auto", s.delay_auto);
            r.integer("delay_ms", s.delay_ms, 0, 400);
            r.boolean("video_sync", s.video_sync);
            r.integer("av_offset_ms", s.av_offset_ms, -200, 200);
        }
    }
    {
        std::map<std::string, std::string> kv;
        if (read_kv(file_in("video.conf"), "GLINT-VIDEO-1", kv, warnings)) {
            Reader r{kv, warnings, "video.conf"};
            std::string v;
            r.text("decoder", v);
            if (v == "cpu") s.gpu_decode = false;
            v.clear();
            r.text("conversion", v);
            if (v == "cpu") s.gpu_convert = false;
            v.clear();
            r.text("latency", v);
            if (v == "balanced") s.latency = 1;
            if (v == "smooth") s.latency = 2;
        }
    }
    {
        std::map<std::string, std::string> kv;
        if (read_kv(file_in("picture.conf"), "GLINT-PICTURE-1", kv, warnings)) {
            Reader r{kv, warnings, "picture.conf"};
            PictureSettings& e = s.enhance;
            r.integer("profile", e.profile, 0, 4);
            r.boolean("protect", e.protect);
            r.real("budget_ms", e.budget_ms, 0.5f, 10.0f);
            r.integer("upscaler", e.upscaler, 0, 3);
            r.real("sharpness", e.sharpness, 0.0f, 1.0f);
            r.real("denoise", e.denoise, 0.0f, 1.0f);
            r.real("deband", e.deband, 0.0f, 1.0f);
            r.real("dither", e.dither, 0.0f, 1.0f);
            r.real("exposure", e.exposure, -2.0f, 2.0f);
            r.real("saturation", e.saturation, 0.0f, 2.0f);
            r.real("shadows", e.shadows, 0.0f, 1.0f);
            r.text("active", s.picture_active);
            // presetN=name|upscaler|sharpness|denoise|deband|dither|exposure|saturation|shadows
            for (int i = 1; i <= kMaxPicturePresets; i++) {
                std::string line;
                r.text(("preset" + std::to_string(i)).c_str(), line);
                if (line.empty()) continue;
                std::vector<std::string> f;
                std::stringstream ss(line);
                for (std::string part; std::getline(ss, part, '|');) f.push_back(part);
                PicturePreset p;
                if (f.size() != 9 || f[0].empty()) {
                    warnings.push_back("picture.conf: preset" + std::to_string(i) + " is not readable");
                    continue;
                }
                p.name = f[0];
                PictureSettings& v = p.values;
                auto num = [&](const std::string& t, float lo, float hi, float fallback) {
                    char* end = nullptr;
                    double n = std::strtod(t.c_str(), &end);
                    return (end && *end == 0 && std::isfinite(n) && n >= lo && n <= hi) ? (float)n : fallback;
                };
                v.upscaler = (int)num(f[1], 0, 3, (float)v.upscaler);
                v.sharpness = num(f[2], 0, 1, v.sharpness);
                v.denoise = num(f[3], 0, 1, v.denoise);
                v.deband = num(f[4], 0, 1, v.deband);
                v.dither = num(f[5], 0, 1, v.dither);
                v.exposure = num(f[6], -2, 2, v.exposure);
                v.saturation = num(f[7], 0, 2, v.saturation);
                v.shadows = num(f[8], 0, 1, v.shadows);
                v.profile = 4;   // Custom
                s.picture_presets.push_back(p);
            }
        }
    }
    {
        std::map<std::string, std::string> kv;
        if (read_kv(file_in("capture.conf"), "GLINT-CAPTURE-1", kv, warnings)) {
            Reader r{kv, warnings, "capture.conf"};
            r.text("media_dir", s.media_dir);
            if (!s.media_dir.empty() && !fs::u8path(s.media_dir).is_absolute()) {
                warnings.push_back("capture.conf: media_dir must be absolute");
                s.media_dir.clear();
            }
        }
    }
    {
        std::map<std::string, std::string> kv;
        if (read_kv(file_in("window.conf"), "GLINT-WINDOW-1", kv, warnings)) {
            Reader r{kv, warnings, "window.conf"};
            std::string fit;
            r.text("picture", fit);
            if (fit == "fill") s.picture = 1;
            else if (fit == "native") s.picture = 2;
            r.integer("x", s.window_x, -32000, 32000);
            r.integer("y", s.window_y, -32000, 32000);
            r.integer("width", s.window_w, 320, 16384);
            r.integer("height", s.window_h, 240, 16384);
            r.boolean("maximized", s.maximized);
            r.boolean("fullscreen", s.fullscreen);
            std::string theme;
            r.text("theme", theme);
            if (theme == "light") s.theme = 1;
        }
    }
}

bool save_settings(const AppSettings& s, std::string& error) {
    for (const std::string* v : {&s.console_url, &s.media_dir})
        if (!one_line(*v)) {
            error = "A setting contains a line break and was not saved.";
            return false;
        }
    for (const std::string& v : s.recent)
        if (!one_line(v)) {
            error = "A setting contains a line break and was not saved.";
            return false;
        }
    char num[64];
    auto f = [&](float v) {
        std::snprintf(num, sizeof(num), "%.3f", v);
        return std::string(num);
    };
    std::string dir = settings_dir();
    auto path = [&](const char* name) { return (fs::u8path(dir) / fs::u8path(name)).u8string(); };
    std::ostringstream a;
    a << "GLINT-CONSOLE-1\naddress=" << s.console_url << "\n";
    for (size_t i = 0; i < s.recent.size() && i < 4; i++) a << "recent" << i + 1 << "=" << s.recent[i] << "\n";
    if (!write_file_atomic(path("console.conf"), a.str(), error)) return false;
    std::ostringstream b;
    b << "GLINT-AUDIO-1\nsound=" << (s.sound ? 1 : 0) << "\nvolume=" << f(s.volume)
      << "\ndelay_auto=" << (s.delay_auto ? 1 : 0) << "\ndelay_ms=" << s.delay_ms
      << "\nvideo_sync=" << (s.video_sync ? 1 : 0) << "\nav_offset_ms=" << s.av_offset_ms << "\n";
    if (!write_file_atomic(path("audio.conf"), b.str(), error)) return false;
    std::ostringstream v;
    v << "GLINT-VIDEO-1\ndecoder=" << (s.gpu_decode ? "gpu" : "cpu") << "\nconversion="
      << (s.gpu_convert ? "gpu" : "cpu") << "\nlatency="
      << (s.latency == 2 ? "smooth" : s.latency == 1 ? "balanced" : "lowest") << "\n";
    if (!write_file_atomic(path("video.conf"), v.str(), error)) return false;
    const PictureSettings& pe = s.enhance;
    std::ostringstream q;
    q << "GLINT-PICTURE-1\nprofile=" << pe.profile << "\nprotect=" << (pe.protect ? 1 : 0)
      << "\nbudget_ms=" << f(pe.budget_ms) << "\nupscaler=" << pe.upscaler << "\nsharpness=" << f(pe.sharpness)
      << "\ndenoise=" << f(pe.denoise) << "\ndeband=" << f(pe.deband) << "\ndither=" << f(pe.dither)
      << "\nexposure=" << f(pe.exposure) << "\nsaturation=" << f(pe.saturation) << "\nshadows=" << f(pe.shadows)
      << "\n";
    if (!s.picture_active.empty()) q << "active=" << s.picture_active << "\n";
    int n = 0;
    for (const PicturePreset& p : s.picture_presets) {
        if (++n > kMaxPicturePresets) break;
        std::string name;
        for (char ch : p.name)   // the separator and line breaks can't be in a name
            if (ch != '|' && ch != '\n' && ch != '\r') name += ch;
        const PictureSettings& v = p.values;
        q << "preset" << n << "=" << name << "|" << v.upscaler << "|" << f(v.sharpness) << "|" << f(v.denoise) << "|"
          << f(v.deband) << "|" << f(v.dither) << "|" << f(v.exposure) << "|" << f(v.saturation) << "|"
          << f(v.shadows) << "\n";
    }
    if (!write_file_atomic(path("picture.conf"), q.str(), error)) return false;
    std::ostringstream c;
    c << "GLINT-CAPTURE-1\nmedia_dir=" << s.media_dir << "\n";
    if (!write_file_atomic(path("capture.conf"), c.str(), error)) return false;
    std::ostringstream e;
    e << "GLINT-WINDOW-1\npicture=" << (s.picture == 1 ? "fill" : s.picture == 2 ? "native" : "fit") << "\n";
    if (s.window_w > 0 && s.window_h > 0)
        e << "x=" << s.window_x << "\ny=" << s.window_y << "\nwidth=" << s.window_w << "\nheight=" << s.window_h << "\n";
    e << "maximized=" << (s.maximized ? 1 : 0) << "\nfullscreen=" << (s.fullscreen ? 1 : 0)
      << "\ntheme=" << (s.theme == 1 ? "light" : "dark") << "\n";
    return write_file_atomic(path("window.conf"), e.str(), error);
}

std::vector<std::string> shareable_settings_files() {
    // Never oauth-apps.conf or anything from the vault.
    return {"console.conf", "audio.conf", "video.conf", "picture.conf", "capture.conf", "window.conf",
            "destinations.conf"};
}

} // namespace glint
