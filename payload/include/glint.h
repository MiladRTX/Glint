#ifndef GLINT_H
#define GLINT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>

/*
 * Glint payload.
 *
 * Shared state, HTTP server interface and capture backend interface.
 * The HTTP server (server.c + media_http.c) publishes two framed streams
 * (/frames, /audio) and a control API; the capture backend fills the ring
 * buffers below and the server pumps them out.
 */

#define SL_VERSION        "1.5"   /* shown in the console banner, capture.log and /status.json */
#define SL_MAX_CLIENTS    16
#define SL_MAX_STREAMING   8
#define SL_VIDEO_SLOTS    64
#define SL_AUDIO_SLOTS   256

typedef struct sl_video_slot {
    uint8_t *data;
    size_t   len;
    size_t   cap;
    uint64_t seq;
    uint32_t pts;
    uint32_t key;
} sl_video_slot;

typedef struct sl_audio_slot {
    uint8_t *data;
    size_t   len;
    uint64_t seq;
    uint32_t pts;
} sl_audio_slot;

typedef struct sl_state {
    pthread_mutex_t lock;
    pthread_cond_t  cond;

    /* Stream description. */
    uint8_t  sps[128];
    size_t   sps_len;
    uint8_t  pps[128];
    size_t   pps_len;
    char     platform[16];
    uint16_t width;
    uint16_t height;
    uint32_t fps;
    uint32_t bitrate;

    /* Video ring (SL_VIDEO_SLOTS slots). */
    sl_video_slot video[SL_VIDEO_SLOTS];
    uint64_t video_seq;
    uint64_t video_bytes;
    uint64_t video_frames;
    uint64_t started_ms;
    uint64_t video_generation;

    /* Server. */
    volatile int running;
    int      listen_fd;
    int      client_count;
    int      streaming_count;

    /* One viewer at a time (server.c, sl_viewer_admit). A new viewer takes
     * over and every stream of the previous one ends; viewers it pushed out
     * are refused while it still watches, so two of them can't fight. */
    char     viewer[48];
    uint64_t viewer_epoch;
    int      viewer_streams;
    uint64_t viewer_idle_ms;
    char     viewer_kicked[4][48];
    volatile int keyframe_request;
    volatile int record_requested;

    /* Capture profile. */
    volatile int profile_pending;
    volatile int requested_height;
    volatile int profile_error;
    volatile int restart_request;
    volatile int discontinuity;

    /* Recording. */
    FILE    *recording;
    int      rec_rewrite_headers;
    uint64_t rec_frames;
    uint64_t rec_bytes;
    uint64_t rec_started_ms;
    double   rec_seconds;
    int      rec_error;
    char     recording_name[96];

    /* Audio ring (SL_AUDIO_SLOTS slots). */
    char     audio_codec[16];
    int      audio_channels;
    int      audio_rate;
    int      audio_none;
    sl_audio_slot audio[SL_AUDIO_SLOTS];
    uint64_t audio_seq;
    uint64_t audio_chunks;
    uint64_t audio_bytes;
    uint64_t audio_started_ms;
    uint64_t audio_generation;

    /* Client bookkeeping. */
    pthread_t accept_thread;
    int       accept_active;
    pthread_t threads[SL_MAX_CLIENTS];
    int       thread_active[SL_MAX_CLIENTS];
    int       slot_used[SL_MAX_CLIENTS];
    int       slot_fd[SL_MAX_CLIENTS];
} sl_state;

typedef struct sl_options {
    uint16_t port;
    uint16_t height;
    uint32_t fps;
    uint32_t bitrate;
    int      minqp;
    int      audio;
} sl_options;

extern sl_state g;
extern sl_options sl_opts;

/* ---- server ---- */
int  sl_server_start(uint16_t port, uint32_t bitrate, uint16_t height,
                      uint32_t fps, const uint8_t *sps, size_t sps_len,
                      const uint8_t *pps, size_t pps_len);
void sl_server_stop(void);
void sl_server_wait(void);
int  sl_server_running(void);

/* ---- media ---- */
void sl_h264_publish(const uint8_t *data, size_t len, uint32_t pts);
int  sl_audio_publish(const void *data, size_t len, uint32_t pts);
int  sl_audio_configure(const char *codec, int channels, int rate);
void sl_h264_discontinuity(void);

/* Control flags consumed by the capture loop. */
int  sl_take_keyframe_request(void);
int  sl_take_restart_request(void);
int  sl_take_profile_request(uint16_t *height);
void sl_profile_applied(uint16_t height);
void sl_profile_failed(void);

/* ---- backend ---- */
int  sl_capture_start(void);
void sl_capture_stop(void);
int  sl_capture_failed(void);   /* the encoder refused for good: quit instead of waiting */

/* ---- helpers ---- */
uint64_t sl_now_ms(void);
void sl_sleep_ms(unsigned ms);
const char *sl_data_dir(void);

/* Pop-up on the console (stderr on the host build). error=1 marks a failure
 * already explained to the user, so main() does not add a second banner. */
void sl_notify(int error, const char *fmt, ...);
int  sl_error_notified(void);

/* Embedded web assets (generated from assets/). */
extern const unsigned char SL_DASHBOARD_HTML[];
extern const size_t SL_DASHBOARD_HTML_len;
extern const unsigned char SL_BANNER_PNG[];
extern const size_t SL_BANNER_PNG_len;

#endif /* GLINT_H */




