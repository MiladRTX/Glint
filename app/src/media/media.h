#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace glint {

// %USERPROFILE%\Videos\Glint (the same string on every call).
std::string default_media_dir();

// First picture of a PNG, MKV or MP4 as RGBA. Thread-safe.
bool load_image_rgba(const std::string& path, std::vector<uint8_t>& rgba,
                     int& width, int& height, std::string& error);

// <dir>\Glint-Screenshot-YYYYMMDD-HHMMSS.png, never overwriting. Thread-safe.
bool save_screenshot(const std::string& dir, const std::vector<uint8_t>& rgba,
                     int width, int height, std::string& out_path, std::string& error);

struct GalleryItem {
    std::string path;
    std::string name;
    bool video = false;
    uint64_t size = 0;
    int64_t modified = 0;
    double duration = 0.0;
    bool has_audio = false;
    int width = 0;
    int height = 0;
};

// .mkv, .mp4 and .png files directly in `dir`, newest first.
std::vector<GalleryItem> list_gallery(const std::string& dir);

// Windows change notifications for one folder.
class FolderWatch {
public:
    FolderWatch();
    ~FolderWatch();
    FolderWatch(const FolderWatch&) = delete;
    FolderWatch& operator=(const FolderWatch&) = delete;
    void watch(const std::string& dir);
    bool changed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool delete_gallery_file(const std::string& path, std::string& error);

// Remux to "mp4" or "mkv" next to the source, without re-encoding.
bool export_copy(const std::string& source, const std::string& ext,
                 std::string& out_path, std::string& error);

bool open_with_default_app(const std::string& path);

}
