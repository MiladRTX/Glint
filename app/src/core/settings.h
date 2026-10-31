#pragma once
#include <climits>
#include <string>
#include <vector>

namespace glint {

// %LOCALAPPDATA%\Glint, or $GLINT_SETTINGS_DIR. Created when missing.
std::string settings_dir();

struct PictureSettings {
    int profile = 2;
    bool protect = true;
    float budget_ms = 3.0f;
    int upscaler = 2;
    float sharpness = 0.35f;
    float denoise = 0.0f;
    float deband = 0.25f;
    float dither = 0.5f;
    float exposure = 0.0f;
    float saturation = 1.0f;
    float shadows = 0.0f;
    bool operator==(const PictureSettings& other) const;
};

struct PicturePreset {
    std::string name;
    PictureSettings values;
    bool operator==(const PicturePreset& other) const;
};

constexpr int kMaxPicturePresets = 20;

struct AppSettings {
    std::string console_url;
    std::vector<std::string> recent;
    bool sound = true;
    float volume = 1.0f;
    bool delay_auto = true;
    int delay_ms = 250;
    bool video_sync = true;
    int av_offset_ms = 0;
    std::string media_dir;
    bool gpu_decode = true;
    bool gpu_convert = true;
    int latency = 0;
    PictureSettings enhance;
    std::vector<PicturePreset> picture_presets;
    std::string picture_active;
    int picture = 0;
    int window_x = INT_MIN;
    int window_y = INT_MIN;
    int window_w = 0;
    int window_h = 0;
    bool maximized = false;
    bool fullscreen = false;
    int theme = 0;
    bool operator==(const AppSettings& other) const;
    bool operator!=(const AppSettings& other) const;
};

// Defaults, then every settings file; problems become warnings.
void load_settings(AppSettings& settings, std::vector<std::string>& warnings);
// Writes every settings file atomically.
bool save_settings(const AppSettings& settings, std::string& error);

}
