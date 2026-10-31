#include "decoder.h"
#include "convert.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "log.h"

namespace glint {

namespace {

void ffmpeg_log(void*, int level, const char* fmt, va_list args)
{
    if (level > AV_LOG_ERROR) return;
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) >= 200) return;   // a broken stream must not flood the log
    char text[1024];
    std::vsnprintf(text, sizeof(text), fmt, args);
    size_t n = std::strlen(text);
    while (n && (text[n - 1] == 10 || text[n - 1] == 13)) text[--n] = 0;   // trailing newline
    if (n) logf("ffmpeg: %s", text);
}

}  // namespace

void ffmpeg_setup()
{
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_ERROR);
        av_log_set_callback(ffmpeg_log);
    });
}

// --- colour conversion -------------------------------------------------------------

void frame_colour(const AVFrame* f, bool& bt709, bool& full_range)
{
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: bt709 = true; break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: bt709 = false; break;
    default: bt709 = f->height >= 720; break;
    }
    full_range = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
                 f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P;
}

__attribute__((optimize("O3"))) void yuv420_to_rgba(const uint8_t* y, int ys, const uint8_t* u, int us, const uint8_t* v, int vs, bool nv12,
                    int w, int h, bool bt709, bool full, uint8_t* out)
{
    // Fixed point: luma scaled by 2^19; chroma deltas carry a factor 8 from
    // the bilinear weights (3:1 vertically, 1:1 horizontally) times 2^16.
    const double luma_gain = full ? 1.0 : 255.0 / 219.0;
    const double chroma_gain = full ? 1.0 : 255.0 / 224.0;
    const double rv = bt709 ? 1.5748 : 1.402, gu = bt709 ? 0.187324 : 0.344136;
    const double gv = bt709 ? 0.468124 : 0.714136, bu = bt709 ? 1.8556 : 1.772;
    const int32_t RV = (int32_t)(rv * chroma_gain * 65536.0 + 0.5);
    const int32_t GU = (int32_t)(gu * chroma_gain * 65536.0 + 0.5);
    const int32_t GV = (int32_t)(gv * chroma_gain * 65536.0 + 0.5);
    const int32_t BU = (int32_t)(bu * chroma_gain * 65536.0 + 0.5);
    const int32_t round = 1 << 18;
    const int y_off = full ? 0 : 16;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;

    const int32_t YG = (int32_t)std::lround(luma_gain * 65536.0 * 8.0);
    const int32_t YB = -YG * y_off;

    std::vector<int32_t> urow((size_t)cw + 1), vrow((size_t)cw + 1);
    std::vector<int32_t> cr((size_t)w + 1), cg((size_t)w + 1), cb((size_t)w + 1);
    for (int row = 0; row < h; row++) {
        // Vertical chroma position (row + 0.5) / 2 - 0.5: weights 3/4 and 1/4.
        int k = row / 2;
        int other = (row & 1) ? std::min(k + 1, ch - 1) : std::max(k - 1, 0);
        const uint8_t* ua = u + (size_t)k * us;
        const uint8_t* ub = u + (size_t)other * us;
        if (nv12) {
            for (int i = 0; i < cw; i++) {
                urow[i] = 3 * ua[2 * i] + ub[2 * i];   // x4
                vrow[i] = 3 * ua[2 * i + 1] + ub[2 * i + 1];
            }
        } else {
            const uint8_t* va = v + (size_t)k * vs;
            const uint8_t* vb = v + (size_t)other * vs;
            for (int i = 0; i < cw; i++) {
                urow[i] = 3 * ua[i] + ub[i];
                vrow[i] = 3 * va[i] + vb[i];
            }
        }
        urow[cw] = urow[cw - 1];
        vrow[cw] = vrow[cw - 1];

        // Chroma terms per output pixel (even columns co-sited, odd ones halfway).
        for (int i = 0; i < cw; i++) {
            int32_t du0 = urow[i] * 2 - 1024, dv0 = vrow[i] * 2 - 1024;
            int32_t du1 = urow[i] + urow[i + 1] - 1024, dv1 = vrow[i] + vrow[i + 1] - 1024;
            cr[2 * i] = RV * dv0 + round;
            cg[2 * i] = round - GU * du0 - GV * dv0;
            cb[2 * i] = BU * du0 + round;
            cr[2 * i + 1] = RV * dv1 + round;
            cg[2 * i + 1] = round - GU * du1 - GV * dv1;
            cb[2 * i + 1] = BU * du1 + round;
        }

        const uint8_t* yl = y + (size_t)row * ys;
        uint8_t* o = out + (size_t)row * w * 4;
        for (int x = 0; x < w; x++) {
            int32_t l = YG * (int32_t)yl[x] + YB;
            int32_t r = (l + cr[x]) >> 19, g = (l + cg[x]) >> 19, b = (l + cb[x]) >> 19;
            o[4 * x] = (uint8_t)std::min(255, std::max(0, r));
            o[4 * x + 1] = (uint8_t)std::min(255, std::max(0, g));
            o[4 * x + 2] = (uint8_t)std::min(255, std::max(0, b));
            o[4 * x + 3] = 255;
        }
    }
}

bool frame_to_rgba(const AVFrame* f, std::vector<uint8_t>& out, SwsContext** cache)
{
    if (!f || f->width <= 0 || f->height <= 0) return false;
    const int w = f->width, h = f->height;
    out.resize((size_t)w * h * 4);
    bool bt709, full;
    frame_colour(f, bt709, full);
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        yuv420_to_rgba(f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2], false, w,
                       h, bt709, full, out.data());
        return true;
    }
    if (f->format == AV_PIX_FMT_NV12) {
        yuv420_to_rgba(f->data[0], f->linesize[0], f->data[1], f->linesize[1], nullptr, 0, true, w, h, bt709, full,
                       out.data());
        return true;
    }
    SwsContext* local = nullptr;
    SwsContext** slot = cache ? cache : &local;
    *slot = sws_getCachedContext(*slot, w, h, (AVPixelFormat)f->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                 nullptr, nullptr, nullptr);
    if (!*slot) return false;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)f->format);
    if (desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB) && !(desc->flags & AV_PIX_FMT_FLAG_PAL)) {
        const int* coef = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
        sws_setColorspaceDetails(*slot, coef, full ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16,
                                 1 << 16);
    }
    uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {w * 4, 0, 0, 0};
    int rows = sws_scale(*slot, f->data, f->linesize, 0, h, dst, dst_stride);
    if (local) sws_freeContext(local);
    if (rows != h) return false;
    // Formats without alpha come out opaque; make sure of it.
    if (!desc || !(desc->flags & AV_PIX_FMT_FLAG_ALPHA))
        for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
    return true;
}

bool picture_to_rgba(const Picture& p, std::vector<uint8_t>& rgba)
{
    if (p.width <= 0 || p.height <= 0) return false;
    const size_t w = (size_t)p.width, h = (size_t)p.height;
    const size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    switch (p.format) {
    case Picture::Format::Rgba:
        if (p.data.size() < w * h * 4) return false;
        rgba.assign(p.data.begin(), p.data.begin() + (ptrdiff_t)(w * h * 4));
        return true;
    case Picture::Format::Nv12:
        if (p.data.size() < w * h + cw * 2 * ch) return false;
        rgba.resize(w * h * 4);
        yuv420_to_rgba(p.data.data(), (int)w, p.data.data() + w * h, (int)(cw * 2), nullptr, 0, true, (int)w,
                       (int)h, p.bt709, p.full_range, rgba.data());
        return true;
    case Picture::Format::I420:
        if (p.data.size() < w * h + cw * ch * 2) return false;
        rgba.resize(w * h * 4);
        yuv420_to_rgba(p.data.data(), (int)w, p.data.data() + w * h, (int)cw, p.data.data() + w * h + cw * ch,
                       (int)cw, false, (int)w, (int)h, p.bt709, p.full_range, rgba.data());
        return true;
    }
    return false;
}

// --- decoder -------------------------------------------------------------------------

struct H264Decoder::Impl {
    AVCodecContext* ctx = nullptr;
    AVBufferRef* hw_device = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* sw = nullptr;
    SwsContext* sws = nullptr;
    Output output = Output::Rgba;
    std::string description;
    bool want_gpu = false;
    bool hw_ready = false;      // a D3D11VA device exists
    bool hw_declined = false;   // the hwaccel refused the stream
    bool hw_active = false;
    int gpu_failures = 0;
    int last_w = 0, last_h = 0;

    void describe()
    {
        if (hw_active || (hw_ready && !hw_declined)) description = "GPU (D3D11VA)";
        else if (!want_gpu) description = "CPU";
        else if (!hw_ready) description = "CPU (no D3D11VA device)";
        else description = "CPU (the GPU declined this stream)";
    }

    static AVPixelFormat pick_format(AVCodecContext* c, const AVPixelFormat* fmts)
    {
        Impl* self = (Impl*)c->opaque;
        AVPixelFormat soft = AV_PIX_FMT_NONE;
        for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == AV_PIX_FMT_D3D11 && self->hw_ready) {
                self->hw_declined = false;
                self->describe();
                return *p;
            }
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
            if (soft == AV_PIX_FMT_NONE && d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) soft = *p;
        }
        if (self->hw_ready) {
            self->hw_declined = true;
            self->hw_active = false;
            self->describe();
        }
        return soft;
    }

    void release()
    {
        if (ctx) avcodec_free_context(&ctx);
        if (hw_device) av_buffer_unref(&hw_device);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (sw) av_frame_free(&sw);
        if (sws) sws_freeContext(sws);
        sws = nullptr;
        hw_ready = hw_declined = hw_active = false;
        gpu_failures = 0;
    }

    void to_picture(const AVFrame* f, Picture& pic)
    {
        const int w = f->width, h = f->height;
        const size_t cw = (size_t)(w + 1) / 2, ch = (size_t)(h + 1) / 2;
        pic.width = w;
        pic.height = h;
        frame_colour(f, pic.bt709, pic.full_range);
        if (output == Output::Yuv && f->format == AV_PIX_FMT_NV12) {
            pic.format = Picture::Format::Nv12;
            pic.data.resize((size_t)w * h + cw * 2 * ch);
            uint8_t* dst = pic.data.data();
            for (int r = 0; r < h; r++) std::memcpy(dst + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], w);
            dst += (size_t)w * h;
            for (size_t r = 0; r < ch; r++)
                std::memcpy(dst + r * cw * 2, f->data[1] + r * f->linesize[1], cw * 2);
            return;
        }
        if (output == Output::Yuv && (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P)) {
            pic.format = Picture::Format::I420;
            pic.data.resize((size_t)w * h + cw * ch * 2);
            uint8_t* dst = pic.data.data();
            for (int r = 0; r < h; r++) std::memcpy(dst + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], w);
            dst += (size_t)w * h;
            for (size_t r = 0; r < ch; r++) std::memcpy(dst + r * cw, f->data[1] + r * f->linesize[1], cw);
            dst += cw * ch;
            for (size_t r = 0; r < ch; r++) std::memcpy(dst + r * cw, f->data[2] + r * f->linesize[2], cw);
            return;
        }
        pic.format = Picture::Format::Rgba;
        frame_to_rgba(f, pic.data, &sws);
    }
};

H264Decoder::H264Decoder() : impl_(new Impl) {}

H264Decoder::~H264Decoder()
{
    close();
}

bool H264Decoder::open(std::string& error, bool prefer_gpu)
{
    close();
    ffmpeg_setup();
    Impl& d = *impl_;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        error = "This build has no H.264 decoder.";
        return false;
    }
    d.want_gpu = prefer_gpu;
    d.ctx = avcodec_alloc_context3(codec);
    d.pkt = av_packet_alloc();
    d.frame = av_frame_alloc();
    d.sw = av_frame_alloc();
    if (!d.ctx || !d.pkt || !d.frame || !d.sw) {
        d.release();
        error = "Out of memory while opening the decoder.";
        return false;
    }
    if (prefer_gpu && av_hwdevice_ctx_create(&d.hw_device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
        d.hw_ready = true;
        d.ctx->hw_device_ctx = av_buffer_ref(d.hw_device);
    }
    d.ctx->opaque = &d;
    d.ctx->get_format = &Impl::pick_format;
    d.ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // Slice threads only: frame threads would hold pictures back.
    d.ctx->thread_type = FF_THREAD_SLICE;
    d.ctx->thread_count = d.hw_ready ? 1 : 0;
    if (avcodec_open2(d.ctx, codec, nullptr) < 0) {
        d.release();
        error = "The H.264 decoder could not be opened.";
        return false;
    }
    d.describe();
    return true;
}

void H264Decoder::close()
{
    impl_->release();
}

void H264Decoder::set_output(Output output)
{
    impl_->output = output;
}

bool H264Decoder::decode(const uint8_t* data, size_t size, uint32_t pts, Picture& picture, std::string& error,
                         const FrameTap& on_frame)
{
    Impl& d = *impl_;
    error.clear();
    if (!d.ctx) {
        error = "The decoder is not open.";
        return false;
    }
    if (!data || size == 0) return false;
    av_packet_unref(d.pkt);
    if (av_new_packet(d.pkt, (int)size) < 0) {
        error = "Out of memory.";
        return false;
    }
    std::memcpy(d.pkt->data, data, size);
    d.pkt->pts = pts;
    d.pkt->dts = AV_NOPTS_VALUE;

    bool using_gpu = d.hw_ready && !d.hw_declined;
    int rc = avcodec_send_packet(d.ctx, d.pkt);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        error = "The decoder rejected the video data.";
        if (using_gpu) d.gpu_failures++;
        return false;
    }
    bool got = false;
    for (;;) {
        rc = avcodec_receive_frame(d.ctx, d.frame);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) {
            error = "Decoding a video frame failed.";
            if (d.hw_ready && !d.hw_declined) d.gpu_failures++;
            break;
        }
        AVFrame* src = d.frame;
        if (d.frame->format == AV_PIX_FMT_D3D11) {
            av_frame_unref(d.sw);
            if (av_hwframe_transfer_data(d.sw, d.frame, 0) < 0) {
                error = "Copying a frame from the GPU failed.";
                d.gpu_failures++;
                av_frame_unref(d.frame);
                continue;
            }
            av_frame_copy_props(d.sw, d.frame);
            d.sw->width = d.frame->width;
            d.sw->height = d.frame->height;
            src = d.sw;
            d.gpu_failures = 0;
            if (!d.hw_active) {
                d.hw_active = true;
                d.describe();
            }
        }
        if (src->width > 0 && src->height > 0) {
            if (on_frame) on_frame(src);
            d.to_picture(src, picture);
            d.last_w = src->width;
            d.last_h = src->height;
            got = true;
        }
        av_frame_unref(d.frame);
    }
    if (got) error.clear();
    return got;
}

const std::string& H264Decoder::description() const
{
    return impl_->description;
}

int H264Decoder::last_width() const
{
    return impl_->last_w;
}

int H264Decoder::last_height() const
{
    return impl_->last_h;
}

bool H264Decoder::hardware_active() const
{
    return impl_->hw_active;
}

int H264Decoder::gpu_failures() const
{
    return impl_->gpu_failures;
}

}
