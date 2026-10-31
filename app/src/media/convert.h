#pragma once
// Internal colour conversion shared by the decoder, the media player and the
// gallery loader. Implemented in decoder.cpp.
#include <cstdint>
#include <vector>

struct AVFrame;
struct SwsContext;

namespace glint {

// Once per process: FFmpeg's own messages go to the log (errors only).
void ffmpeg_setup();

// Matrix and range of a decoded frame, by the rules of the live decoder.
void frame_colour(const AVFrame* frame, bool& bt709, bool& full_range);

// 8-bit 4:2:0 (I420 planes, or NV12 with `u` holding interleaved U/V) to
// RGBA. Chroma is bilinear, left-sited, like the UI's GPU converter.
void yuv420_to_rgba(const uint8_t* y, int y_stride, const uint8_t* u, int u_stride, const uint8_t* v,
                    int v_stride, bool nv12, int width, int height, bool bt709, bool full_range, uint8_t* out);

// Any software frame to tightly packed RGBA. `sws` caches a scaler for formats
// other than 4:2:0 (pass null to use a temporary one).
bool frame_to_rgba(const AVFrame* frame, std::vector<uint8_t>& out, SwsContext** sws);

}
