#include "decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace glint {
namespace {

// Offer D3D11 when the decoder lists it; otherwise take its software format.
AVPixelFormat pick_format(AVCodecContext* ctx, const AVPixelFormat* formats) {
    const int wanted = *static_cast<const int*>(ctx->opaque);
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; f++)
        if (*f == wanted) return *f;
    return avcodec_default_get_format(ctx, formats);
}

std::string av_text(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

} // namespace

H264Decoder::~H264Decoder() {
    close();
}

bool H264Decoder::open(std::string& error, bool prefer_gpu) {
    av_log_set_level(AV_LOG_QUIET);
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        error = "FFmpeg H.264 decoder unavailable";
        return false;
    }

    context_ = avcodec_alloc_context3(codec);
    if (!context_) {
        error = "Cannot allocate H.264 decoder";
        return false;
    }

    context_->thread_count = 1;
    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context_->flags2 |= AV_CODEC_FLAG2_FAST;

    hw_format_ = -1;
    hw_active_ = false;
    hw_errors_ = 0;
    description_ = "CPU";
    if (prefer_gpu) {
        int ret = av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
        if (ret >= 0) {
            context_->hw_device_ctx = av_buffer_ref(hw_device_);
            hw_format_ = AV_PIX_FMT_D3D11;
            context_->opaque = &hw_format_;
            context_->get_format = pick_format;
            description_ = "D3D11VA GPU";   // confirmed by the first decoded frame
        } else {
            description_ = "CPU (no D3D11VA device: " + av_text(ret) + ")";
        }
    }

    if (avcodec_open2(context_, codec, nullptr) < 0) {
        error = "Cannot open H.264 decoder";
        close();
        return false;
    }

    frame_ = av_frame_alloc();
    sw_frame_ = av_frame_alloc();
    if (!frame_ || !sw_frame_) {
        error = "Cannot allocate decoder frame";
        close();
        return false;
    }
    return true;
}

void H264Decoder::close() {
    if (scale_) {
        sws_freeContext(scale_);
        scale_ = nullptr;
    }
    if (frame_) av_frame_free(&frame_);
    if (sw_frame_) av_frame_free(&sw_frame_);
    if (context_) avcodec_free_context(&context_);
    if (hw_device_) av_buffer_unref(&hw_device_);
    last_width_ = last_height_ = 0;
    scale_width_ = scale_height_ = 0;
    scale_format_ = -1;
    hw_active_ = false;
}

namespace {

bool frame_is_709(const AVFrame* f) {
    if (f->colorspace == AVCOL_SPC_BT709) return true;
    if (f->colorspace == AVCOL_SPC_BT470BG || f->colorspace == AVCOL_SPC_SMPTE170M) return false;
    return f->height >= 720;   // unspecified: HD content is BT.709
}

bool frame_is_full_range(const AVFrame* f) {
    return f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
}

// sws with the right matrix and range (swscale defaults to BT.601 otherwise).
void set_matrix(SwsContext* sws, bool bt709, bool full_range) {
    const int* src = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    const int* dst = sws_getCoefficients(SWS_CS_DEFAULT);
    sws_setColorspaceDetails(sws, src, full_range ? 1 : 0, dst, 1, 0, 1 << 16, 1 << 16);
}

void copy_plane(uint8_t* dst, const uint8_t* src, int src_stride, int row_bytes, int rows) {
    for (int y = 0; y < rows; y++)
        std::memcpy(dst + (size_t)y * row_bytes, src + (size_t)y * src_stride, (size_t)row_bytes);
}

} // namespace

bool picture_to_rgba(const Picture& pic, std::vector<uint8_t>& rgba) {
    if (pic.width <= 0 || pic.height <= 0) return false;
    if (pic.format == Picture::Format::Rgba) {
        rgba = pic.data;
        return true;
    }
    const int w = pic.width, h = pic.height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    const uint8_t* planes[4] = {pic.data.data(), nullptr, nullptr, nullptr};
    int strides[4] = {w, 0, 0, 0};
    AVPixelFormat fmt;
    if (pic.format == Picture::Format::Nv12) {
        fmt = AV_PIX_FMT_NV12;
        planes[1] = pic.data.data() + (size_t)w * h;
        strides[1] = cw * 2;
    } else {
        fmt = AV_PIX_FMT_YUV420P;
        planes[1] = pic.data.data() + (size_t)w * h;
        planes[2] = planes[1] + (size_t)cw * ch;
        strides[1] = strides[2] = cw;
    }
    SwsContext* sws = sws_getContext(w, h, fmt, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) return false;
    set_matrix(sws, pic.bt709, pic.full_range);
    rgba.resize((size_t)w * h * 4);
    uint8_t* dst[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {w * 4, 0, 0, 0};
    sws_scale(sws, planes, strides, 0, h, dst, dst_stride);
    sws_freeContext(sws);
    return true;
}

bool H264Decoder::decode(const uint8_t* data, size_t size, uint32_t pts, Picture& picture,
                         std::string& error, const FrameTap& on_frame) {
    if (!context_ || !frame_) {
        error = "Decoder is not open";
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        error = "Cannot allocate video packet";
        return false;
    }
    if (av_new_packet(packet, static_cast<int>(size)) < 0) {
        av_packet_free(&packet);
        error = "Cannot allocate video packet buffer";
        return false;
    }
    std::memcpy(packet->data, data, size);
    packet->pts = pts;

    int sent = avcodec_send_packet(context_, packet);
    av_packet_free(&packet);
    if (sent < 0 && sent != AVERROR(EAGAIN)) {
        if (hw_format_ >= 0) hw_errors_++;
        error = "Decoder rejected a packet";
        return false;
    }

    bool produced = false;
    for (;;) {
        int received = avcodec_receive_frame(context_, frame_);
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
        if (received < 0) {
            if (hw_format_ >= 0) hw_errors_++;
            error = "Decoder failed";
            break;
        }

        // GPU frames live in D3D11 textures: copy to system memory (NV12).
        AVFrame* src = frame_;
        if (frame_->format == hw_format_ && hw_format_ >= 0) {
            av_frame_unref(sw_frame_);
            if (av_hwframe_transfer_data(sw_frame_, frame_, 0) < 0) {
                hw_errors_++;
                av_frame_unref(frame_);
                error = "Cannot read the GPU-decoded frame";
                continue;
            }
            av_frame_copy_props(sw_frame_, frame_);
            src = sw_frame_;
            hw_active_ = true;
            hw_errors_ = 0;
        } else if (hw_format_ >= 0 && !hw_active_) {
            description_ = "CPU (GPU declined this stream)";
        }

        const int width = src->width, height = src->height;
        if (width <= 0 || height <= 0) {
            av_frame_unref(frame_);
            continue;
        }

        if (on_frame) on_frame(src);

        const int format = static_cast<int>(src->format);
        const bool bt709 = frame_is_709(src), full = frame_is_full_range(src);
        picture.width = width;
        picture.height = height;
        picture.bt709 = bt709;
        picture.full_range = full;
        const bool yuv_ok = format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_YUV420P || format == AV_PIX_FMT_YUVJ420P;
        if (output_ == Output::Yuv && yuv_ok) {
            // Planes as they are; the GPU does the colour conversion.
            const int cw = (width + 1) / 2, ch = (height + 1) / 2;
            if (format == AV_PIX_FMT_NV12) {
                picture.format = Picture::Format::Nv12;
                picture.data.resize((size_t)width * height + (size_t)cw * 2 * ch);
                copy_plane(picture.data.data(), src->data[0], src->linesize[0], width, height);
                copy_plane(picture.data.data() + (size_t)width * height, src->data[1], src->linesize[1], cw * 2, ch);
            } else {
                picture.format = Picture::Format::I420;
                picture.data.resize((size_t)width * height + (size_t)cw * ch * 2);
                uint8_t* p = picture.data.data();
                copy_plane(p, src->data[0], src->linesize[0], width, height);
                copy_plane(p + (size_t)width * height, src->data[1], src->linesize[1], cw, ch);
                copy_plane(p + (size_t)width * height + (size_t)cw * ch, src->data[2], src->linesize[2], cw, ch);
            }
        } else {
            const int matrix = (bt709 ? 2 : 0) | (full ? 1 : 0);
            if (!scale_ || scale_width_ != width || scale_height_ != height || scale_format_ != format ||
                scale_matrix_ != matrix) {
                if (scale_) sws_freeContext(scale_);
                scale_ = sws_getContext(width, height, static_cast<AVPixelFormat>(format), width, height,
                                        AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                scale_width_ = width;
                scale_height_ = height;
                scale_format_ = format;
                scale_matrix_ = matrix;
                if (!scale_) {
                    av_frame_unref(frame_);
                    error = "Cannot create colour converter";
                    break;
                }
                set_matrix(scale_, bt709, full);
            }
            picture.format = Picture::Format::Rgba;
            picture.data.resize(static_cast<size_t>(width) * height * 4);
            uint8_t* destination[4] = {picture.data.data(), nullptr, nullptr, nullptr};
            int lines[4] = {width * 4, 0, 0, 0};
            sws_scale(scale_, src->data, src->linesize, 0, height, destination, lines);
        }
        last_width_ = width;
        last_height_ = height;
        produced = true;
        if (src == sw_frame_) av_frame_unref(sw_frame_);
        av_frame_unref(frame_);
    }
    return produced;
}

} // namespace glint
