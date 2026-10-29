#include "glint.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#if !defined(SL_PS5) || defined(SL_TEST_CAPTURE)

/*
 * Host capture backend used to exercise the HTTP surface, ring buffers,
 * recording and RTP packetisation without a console. It publishes a
 * syntactically valid Annex-B stream (AUD + IDR/non-IDR NAL units) and
 * PCM audio. The bytes are not meant to decode as a picture; use the PS5
 * backend for real capture.
 */

static pthread_t capture_thread;
static int capture_active;

static const uint8_t HOST_SPS[] = {
    0x67, 0x42, 0xc0, 0x1e, 0xda, 0x02, 0x80, 0xb7,
    0xfe, 0x5c, 0x05, 0x05, 0x05, 0x02
};
static const uint8_t HOST_PPS[] = { 0x68, 0xce, 0x3c, 0x80 };

static size_t put_startcode(uint8_t *out, size_t at)
{
    out[at + 0] = 0;
    out[at + 1] = 0;
    out[at + 2] = 0;
    out[at + 3] = 1;
    return at + 4;
}

static size_t build_frame(uint8_t *out, int key)
{
    size_t n = 0;
    n = put_startcode(out, n);
    out[n++] = 0x09; /* access unit delimiter */
    out[n++] = 0xf0;

    n = put_startcode(out, n);
    out[n++] = key ? 0x65 : 0x41;
    out[n++] = 0x88;
    out[n++] = 0x84;
    for (int i = 0; i < 1200; i++) out[n++] = (uint8_t)(0x55 + (i & 3));
    return n;
}

static void *capture_main(void *arg)
{
    (void)arg;

    if (sl_opts.audio) sl_audio_configure("pcm_f32le", 2, 48000);
    else sl_audio_configure("none", 0, 0);

    if (sl_server_start(sl_opts.port, sl_opts.bitrate, sl_opts.height,
                         sl_opts.fps, HOST_SPS, sizeof(HOST_SPS),
                         HOST_PPS, sizeof(HOST_PPS)) != 0) {
        fprintf(stderr, "Glint: cannot start server on port %d\n", sl_opts.port);
        return NULL;
    }

    fprintf(stderr, "Glint started successfully\n");
    fprintf(stderr, "Streaming and web dashboard ready on port %d\n", sl_opts.port);

    uint8_t frame[2048];
    float samples[960 * 2];
    uint32_t pts = 0;
    uint32_t audio_pts = 0;
    int key = 1;
    int frame_index = 0;
    double phase = 0.0;

    uint64_t next_video = sl_now_ms();
    uint64_t next_audio = next_video;
    const uint64_t video_period = 1000u / sl_opts.fps;
    const uint64_t audio_period = 20; /* 960 frames @ 48 kHz */

    while (g.running) {
        uint64_t now = sl_now_ms();

        if (sl_take_keyframe_request() || sl_take_restart_request()) key = 1;

        uint16_t wanted_height = 0;
        if (sl_take_profile_request(&wanted_height)) {
            sl_server_start(sl_opts.port, sl_opts.bitrate, wanted_height,
                             sl_opts.fps, HOST_SPS, sizeof(HOST_SPS),
                             HOST_PPS, sizeof(HOST_PPS));
            sl_profile_applied(wanted_height);
        }

        if (now >= next_video) {
            size_t len = build_frame(frame, key);
            sl_h264_publish(frame, len, pts);
            pts += 90000u / sl_opts.fps;
            key = 0;
            frame_index++;
            next_video += video_period;
            if (next_video < now) next_video = now + video_period;
        }

        if (sl_opts.audio && now >= next_audio) {
            for (int i = 0; i < 960; i++) {
                float value = (float)sin(phase) * 0.08f;
                phase += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
                if (phase > 6.283185307179586) phase -= 6.283185307179586;
                samples[i * 2 + 0] = value;
                samples[i * 2 + 1] = value;
            }
            sl_audio_publish(samples, sizeof(samples), audio_pts);
            audio_pts += 960;
            next_audio += audio_period;
            if (next_audio < now) next_audio = now + audio_period;
        }

        sl_sleep_ms(1);
    }

    (void)frame_index;
    return NULL;
}

int sl_capture_failed(void)
{
    return 0;   /* the synthetic stream always starts */
}

int sl_capture_start(void)
{
    if (capture_active) return 0;
    if (pthread_create(&capture_thread, NULL, capture_main, NULL) != 0) return -1;
    capture_active = 1;
    return 0;
}

void sl_capture_stop(void)
{
    g.running = 0;
    if (capture_active) {
        pthread_join(capture_thread, NULL);
        capture_active = 0;
    }
}

#endif /* !SL_PS5 || SL_TEST_CAPTURE */


