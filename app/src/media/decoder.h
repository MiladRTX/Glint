#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct SwsContext;

namespace glint {

// One decoded picture, either ready to show (RGBA) or as YUV planes for the
// GPU colour converter. Planes are packed tightly: NV12 = Y then interleaved
// UV; I420 = Y, U, V (chroma planes (w+1)/2 x (h+1)/2).
struct Picture {
    enum class Format { Rgba, Nv12, I420 };
    Format format = Format::Rgba;
    int width = 0;
    int height = 0;
    bool bt709 = true;        // HD matrix (else BT.601)
    bool full_range = false;  // 0-255 luma (else 16-235)
    std::vector<uint8_t> data;
};

// CPU conversion of any Picture to RGBA (screenshots, CPU display path).
bool picture_to_rgba(const Picture& picture, std::vector<uint8_t>& rgba);

class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    // prefer_gpu: decode with D3D11VA when the GPU offers it, else the CPU.
    bool open(std::string& error, bool prefer_gpu = false);
    void close();

    // Rgba: convert on the CPU (as before). Yuv: hand out the planes for the
    // GPU converter (falls back to Rgba for unusual pixel formats).
    enum class Output { Rgba, Yuv };
    void set_output(Output output) { output_ = output; }

    // Feeds one Annex-B access unit. Returns true when at least one frame was
    // decoded into `picture`.
    // `on_frame`, when set, sees every decoded frame (system-memory YUV/NV12,
    // also for GPU decoding) before it is converted for display; it must not
    // keep the pointer (clone it).
    using FrameTap = std::function<void(const AVFrame*)>;
    bool decode(const uint8_t* data, size_t size, uint32_t pts, Picture& picture,
                std::string& error, const FrameTap& on_frame = FrameTap());

    int last_width() const { return last_width_; }
    int last_height() const { return last_height_; }

    // What is decoding right now: "D3D11VA GPU", "CPU", or "CPU (reason)".
    const std::string& description() const { return description_; }
    bool hardware_active() const { return hw_active_; }
    // True after repeated GPU failures; reopen with prefer_gpu = false.
    bool needs_cpu_fallback() const { return hw_errors_ >= 3; }

private:
    AVCodecContext* context_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* sw_frame_ = nullptr;
    SwsContext* scale_ = nullptr;
    int hw_format_ = -1;
    bool hw_active_ = false;
    int hw_errors_ = 0;
    std::string description_ = "CPU";
    int last_width_ = 0;
    int last_height_ = 0;
    int scale_width_ = 0;
    int scale_height_ = 0;
    int scale_format_ = -1;
    int scale_matrix_ = -1;
    Output output_ = Output::Rgba;
};

} // namespace glint
