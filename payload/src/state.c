#include "glint.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define sl_mkdir(path) _mkdir(path)
#else
#  include <unistd.h>
#  define sl_mkdir(path) mkdir(path, 0777)
#endif

sl_state g;

#ifdef SL_PS5
typedef struct notify_request {
    char reserved[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int device, notify_request_t *request, size_t size, int blocking);
#endif

static volatile int g_error_notified;

void sl_notify(int error, const char *fmt, ...)
{
    char text[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (error) g_error_notified = 1;
#ifdef SL_PS5
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", text);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#else
    fprintf(stderr, "notify: %s\n", text);
#endif
}

int sl_error_notified(void)
{
    return g_error_notified;
}

uint64_t sl_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void sl_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

const char *sl_data_dir(void)
{
    const char *dir = getenv("GLINT_DATA_DIR");
    return (dir && *dir) ? dir : "/data/glint";
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int recording_headers_locked(FILE *file)
{
    uint8_t head[4];
    put_be32(head, (uint32_t)g.sps_len);
    if (fwrite(head, 1, 4, file) != 4) return -1;
    if (fwrite(g.sps, 1, g.sps_len, file) != g.sps_len) return -1;
    put_be32(head, (uint32_t)g.pps_len);
    if (fwrite(head, 1, 4, file) != 4) return -1;
    if (fwrite(g.pps, 1, g.pps_len, file) != g.pps_len) return -1;
    return 0;
}

static void recording_stop_locked(void)
{
    if (g.recording) {
        fclose(g.recording);
        g.recording = NULL;
    }
    if (g.rec_started_ms) {
        g.rec_seconds = (double)(sl_now_ms() - g.rec_started_ms) / 1000.0;
        g.rec_started_ms = 0;
    }
    g.rec_rewrite_headers = 0;
}

static int annexb_has_idr(const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i + 3 < len) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if ((data[i + 3] & 0x1f) == 5) return 1;
            i += 3;
        } else if (i + 4 < len && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            if ((data[i + 4] & 0x1f) == 5) return 1;
            i += 4;
        } else {
            i++;
        }
    }
    return 0;
}

void sl_h264_publish(const uint8_t *data, size_t len, uint32_t pts)
{
    if (!data || len == 0 || len > 0x200000) return;

    pthread_mutex_lock(&g.lock);

    uint64_t seq = g.video_seq + 1;
    sl_video_slot *slot = &g.video[(uint32_t)seq & (SL_VIDEO_SLOTS - 1)];

    if (slot->cap < len) {
        size_t cap = 0x4000;
        while (cap < len) cap *= 2;
        uint8_t *grown = realloc(slot->data, cap);
        if (!grown) {
            pthread_mutex_unlock(&g.lock);
            return;
        }
        slot->data = grown;
        slot->cap = cap;
    }

    memcpy(slot->data, data, len);
    slot->len = len;
    slot->pts = pts;
    slot->seq = seq;
    int key = annexb_has_idr(data, len);
    slot->key = (uint32_t)key;

    g.video_bytes += len;
    g.video_frames++;
    g.video_seq = seq;

    /* Start a recording on the first random-access point after the request. */
    if (g.record_requested && g.recording == NULL && key) {
        time_t now = time(NULL);
        snprintf(g.recording_name, sizeof(g.recording_name),
                 "glint-%llu-%llu.h264", (unsigned long long)now,
                 (unsigned long long)g.video_frames);

        char dir[512];
        char path[640];
        snprintf(dir, sizeof(dir), "%s/recordings", sl_data_dir());
        sl_mkdir(sl_data_dir());
        sl_mkdir(dir);
        snprintf(path, sizeof(path), "%s/%s", dir, g.recording_name);

        FILE *file = fopen(path, "wb");
        g.record_requested = 0;
        g.rec_rewrite_headers = 0;
        g.rec_frames = 0;
        g.rec_bytes = 0;
        g.rec_error = 0;
        g.rec_started_ms = sl_now_ms();
        g.rec_seconds = 0.0;
        g.recording = file;

        if (!file) {
            g.rec_error = 1;
        } else if (recording_headers_locked(file) != 0) {
            fclose(file);
            g.recording = NULL;
            g.rec_error = 1;
        }
    }

    if (g.recording) {
        if (g.rec_rewrite_headers && key) {
            g.rec_rewrite_headers = 0;
            if (recording_headers_locked(g.recording) != 0) {
                recording_stop_locked();
                g.rec_error = 1;
            }
        }
        if (g.recording) {
            size_t wrote = fwrite(data, 1, len, g.recording);
            if (wrote == len && fflush(g.recording) == 0) {
                g.rec_frames++;
                g.rec_bytes += len;
            } else {
                recording_stop_locked();
                g.rec_error = 1;
            }
        }
    }

    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

int sl_audio_configure(const char *codec, int channels, int rate)
{
    int changed = 0;

    pthread_mutex_lock(&g.lock);
    if (codec && strcmp(g.audio_codec, codec) != 0) changed = 1;
    if (channels > 0 && g.audio_channels != channels) changed = 1;
    if (rate > 0 && g.audio_rate != rate) changed = 1;
    if (changed) {
        g.audio_generation++;
        pthread_cond_broadcast(&g.cond);
    }
    if (codec) snprintf(g.audio_codec, sizeof(g.audio_codec), "%s", codec);
    if (channels > 0) g.audio_channels = channels;
    if (rate > 0) g.audio_rate = rate;
    g.audio_none = (codec == NULL || strcmp(g.audio_codec, "none") == 0);
    pthread_mutex_unlock(&g.lock);
    return 0;
}

int sl_audio_publish(const void *data, size_t len, uint32_t pts)
{
    if (!data || len == 0 || len > 0x10000) return -1;

    pthread_mutex_lock(&g.lock);
    uint64_t seq = g.audio_seq + 1;
    sl_audio_slot *slot = &g.audio[(uint32_t)seq & (SL_AUDIO_SLOTS - 1)];
    uint8_t *grown = realloc(slot->data, len);
    if (!grown) {
        pthread_mutex_unlock(&g.lock);
        return -1;
    }
    slot->data = grown;
    memcpy(slot->data, data, len);
    slot->len = len;
    slot->seq = seq;
    slot->pts = pts;
    g.audio_seq = seq;
    g.audio_chunks++;
    g.audio_bytes += len;
    if (g.audio_started_ms == 0) g.audio_started_ms = sl_now_ms();
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
    return 0;
}

void sl_h264_discontinuity(void)
{
    pthread_mutex_lock(&g.lock);
    g.discontinuity = 1;
    if (g.recording) g.rec_rewrite_headers = 1;
    g.keyframe_request = 1;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

int sl_take_keyframe_request(void)
{
    int value;
    pthread_mutex_lock(&g.lock);
    value = g.keyframe_request;
    g.keyframe_request = 0;
    pthread_mutex_unlock(&g.lock);
    return value;
}

int sl_take_restart_request(void)
{
    int value;
    pthread_mutex_lock(&g.lock);
    value = g.restart_request;
    g.restart_request = 0;
    pthread_mutex_unlock(&g.lock);
    return value;
}

int sl_take_profile_request(uint16_t *height)
{
    int pending;
    pthread_mutex_lock(&g.lock);
    pending = g.profile_pending;
    if (pending && height) *height = (uint16_t)g.requested_height;
    pthread_mutex_unlock(&g.lock);
    return pending;
}

void sl_profile_applied(uint16_t height)
{
    pthread_mutex_lock(&g.lock);
    g.height = height;
    g.width = (uint16_t)((uint32_t)height * 16 / 9);
    g.profile_pending = 0;
    g.profile_error = 0;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

void sl_profile_failed(void)
{
    pthread_mutex_lock(&g.lock);
    g.profile_pending = 0;
    g.profile_error = 1;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}


