#pragma once

#include <climits>
#include <string>
#include <vector>

namespace glint {

// %LOCALAPPDATA%\Glint (or $GLINT_SETTINGS_DIR for tests); created on demand.
// The first time, settings from the old %LOCALAPPDATA%\Screenlink are copied in.
std::string settings_dir();

// True when `line` is a settings file's first line `header` ("GLINT-...-1"), or
// the same header from before the rename ("SCREENLINK-...-1").
bool is_header(const std::string& line, const char* header);

// Writes <path>.tmp and renames it over <path>, so a crash never leaves a
// half-written file.
bool write_file_atomic(const std::string& path, const std::string& text, std::string& error);

// Everything the user changes that should survive a restart. Secrets are not
// here: stream keys and tokens live in the vault (destinations.h).
struct AppSettings {
    // console.conf
    std::string console_url;
    std::vector<std::string> recent;   // consoles connected to before, newest first (at most 4)
    // audio.conf
    bool sound = true;
    float volume = 1.0f;
    bool delay_auto = true;
    int delay_ms = 250;
    bool video_sync = true;
    int av_offset_ms = 0;
    // capture.conf
    std::string media_dir;          // empty: default folder
    // video.conf
    bool gpu_decode = true;
    bool gpu_convert = true;
    int latency = 0;                // 0 Lowest, 1 Balanced, 2 Smooth (App::Latency)
    // window.conf
    int picture = 0;                // 0 fit, 1 fill, 2 no upscaling
    int window_x = INT_MIN;
    int window_y = INT_MIN;
    int window_w = 0;
    int window_h = 0;
    bool maximized = false;
    bool fullscreen = false;
    int theme = 0;                  // 0 dark (default), 1 light

    bool operator==(const AppSettings& o) const;
    bool operator!=(const AppSettings& o) const { return !(*this == o); }
};

// Missing files keep defaults. A file with a wrong header or bad values is
// ignored value by value, and `warnings` says what was skipped.
void load_settings(AppSettings& settings, std::vector<std::string>& warnings);
bool save_settings(const AppSettings& settings, std::string& error);

// Settings files that may go into a diagnostics bundle (never secrets).
std::vector<std::string> shareable_settings_files();

} // namespace glint
