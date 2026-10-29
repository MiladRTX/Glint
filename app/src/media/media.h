#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace glint {

// Where recordings and screenshots go by default: %USERPROFILE%\Videos\Glint
// (an old Videos\Screenlink folder is renamed to it the first time)
std::string default_media_dir();

// Decodes the first picture of a PNG or a recording into RGBA (gallery previews).
bool load_image_rgba(const std::string& path, std::vector<uint8_t>& rgba, int& width,
                     int& height, std::string& error);

// Writes <dir>/Glint-Screenshot-<timestamp>.png (never overwrites). The picture
// is the decoded game frame, so Glint's own UI is never included.
bool save_screenshot(const std::string& dir, const std::vector<uint8_t>& rgba, int width,
                     int height, std::string& out_path, std::string& error);

struct GalleryItem {
    std::string path;
    std::string name;
    bool video = false;     // false: PNG
    uint64_t size = 0;
    int64_t modified = 0;   // unix seconds
    double duration = 0.0;  // seconds, videos only
    bool has_audio = false;
    int width = 0;
    int height = 0;
};

// .mkv / .mp4 / .png files in `dir`, newest first.
std::vector<GalleryItem> list_gallery(const std::string& dir);

// Watches a folder for files added, removed, renamed or written (Windows
// change notifications, no polling of the files). changed() is true once
// after each burst of changes. Watching a new folder replaces the old one.
class FolderWatch {
public:
    FolderWatch() = default;
    ~FolderWatch();
    FolderWatch(const FolderWatch&) = delete;
    FolderWatch& operator=(const FolderWatch&) = delete;
    void watch(const std::string& dir);
    bool changed();

private:
    void close();
    void* handle_ = nullptr;
    std::string dir_;
};
bool delete_gallery_file(const std::string& path, std::string& error);

// Stream-copies a recording to "mp4" or "mkv" next to the original
// (<name>-export-<timestamp>.<ext>). The original is never modified.
bool export_copy(const std::string& source, const std::string& ext, std::string& out_path,
                 std::string& error);

// Opens a file with the system's default application.
bool open_with_default_app(const std::string& path);

} // namespace glint
