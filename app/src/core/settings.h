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

// Picture enhancement (Settings -> Picture, picture.conf). The profile picks a
// set of values; Custom uses the ones below as the user left them.
struct PictureSettings {
    int profile = 2;          // 0 Off, 1 Light, 2 Balanced, 3 Enhanced, 4 Custom
    bool protect = true;      // step the profile down when the GPU goes over budget
    float budget_ms = 3.0f;   // GPU time allowed for the passes, per frame
    int upscaler = 2;         // 0 Bilinear, 1 Lanczos, 2 FSR 1, 3 AI
    float sharpness = 0.35f;  // 0..1
    float denoise = 0.0f;     // 0..1
    float deband = 0.25f;     // 0..1
    float dither = 0.5f;      // 0..1
    float exposure = 0.0f;    // EV, -2..2
    float saturation = 1.0f;  // 0..2
    float shadows = 0.0f;     // 0..1

    bool operator==(const PictureSettings& o) const {
        return profile == o.profile && protect == o.protect && budget_ms == o.budget_ms && upscaler == o.upscaler &&
               sharpness == o.sharpness && denoise == o.denoise && deband == o.deband && dither == o.dither &&
               exposure == o.exposure && saturation == o.saturation && shadows == o.shadows;
    }
};

// A named set of picture values the user saved (Settings -> Picture, My
// presets). Loading one makes it the Custom profile.
struct PicturePreset {
    std::string name;
    PictureSettings values;
    bool operator==(const PicturePreset& o) const { return name == o.name && values == o.values; }
};
constexpr int kMaxPicturePresets = 20;

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
    // picture.conf
    PictureSettings enhance;
    std::vector<PicturePreset> picture_presets;
    std::string picture_active;     // the user preset last applied, "" for a built-in profile
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
