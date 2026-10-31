#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AVFrame;

namespace glint {

// One decoded image, tightly packed (see Format for the layouts).
struct Picture {
    enum class Format { Rgba, Nv12, I420 };
    Format format = Format::Rgba;
    int width = 0;
    int height = 0;
    bool bt709 = true;
    bool full_range = false;
    std::vector<uint8_t> data;
};

// CPU conversion of any Picture to RGBA (width*height*4, alpha 255).
bool picture_to_rgba(const Picture& picture, std::vector<uint8_t>& rgba);

// H.264 access units (Annex-B) in, pictures out, on D3D11VA or the CPU.
class H264Decoder {
public:
    enum class Output { Rgba, Yuv };
    using FrameTap = std::function<void(const AVFrame*)>;

    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    bool open(std::string& error, bool prefer_gpu = false);
    void close();
    void set_output(Output output);
    bool decode(const uint8_t* data, size_t size, uint32_t pts, Picture& picture,
                std::string& error, const FrameTap& on_frame = FrameTap());
    const std::string& description() const;

    int last_width() const;
    int last_height() const;
    bool hardware_active() const;
    // GPU errors in a row since the last good GPU frame (the App falls back at 3).
    int gpu_failures() const;
    bool needs_cpu_fallback() const { return gpu_failures() >= 3; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
