#include "glint.h"
#include "http_handlers.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define SL_SOCKET SOCKET
#  define sl_close closesocket
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/socket.h>
#  include <unistd.h>
#  define SL_SOCKET int
#  define sl_close close
#endif

/* Client socket options:
 *   level 6 option 1   -> TCP_NODELAY
 *   level 0xffff 0x1001-> SO_SNDBUF = 0x10000
 *   level 0xffff 0x1005-> SO_SNDTIMEO = 5 s
 *   level 0xffff 0x1006-> SO_RCVTIMEO = 1 s
 */
static void configure_client_socket(SL_SOCKET fd)
{
    int one = 1;
    int sndbuf = 0x10000;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf, sizeof(sndbuf));

#ifdef _WIN32
    DWORD send_timeout = 5000;
    DWORD recv_timeout = 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&send_timeout, sizeof(send_timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&recv_timeout, sizeof(recv_timeout));
#else
    struct timeval tv;
    tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 1; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void sl_send_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len) {
#ifdef _WIN32
        int n = send((SOCKET)fd, p, (int)(len > 0x7fffffff ? 0x7fffffff : len), 0);
#else
        ssize_t n = send(fd, p, len, 0);
#endif
        if (n <= 0) {
#ifdef _WIN32
            if (n < 0 && WSAGetLastError() == WSAEINTR) continue;
#else
            if (n < 0 && errno == EINTR) continue;
#endif
            return;
        }
        p += n;
        len -= (size_t)n;
    }
}

void sl_reply(int fd, const char *type, const void *body, size_t len)
{
    char head[264];
    int n = snprintf(head, 256,
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", type, (unsigned long long)len);
    if (n <= 0) return;
    sl_send_all(fd, head, (size_t)n);
    if (len) sl_send_all(fd, body, len);
}

void sl_profile_reply(int fd, int code, const char *message)
{
    char body[196];
    char head[256];
    int blen = snprintf(body, sizeof(body), "{\"message\":\"%s\"}", message);
    int hlen = snprintf(head, sizeof(head),
        "HTTP/1.1 %d Profile\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n", code, blen);
    if (hlen > 0) sl_send_all(fd, head, (size_t)hlen);
    if (blen > 0) sl_send_all(fd, body, (size_t)blen);
}

static void reply_busy(int fd)
{
    static const char busy[] =
        "HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sl_send_all(fd, busy, sizeof(busy) - 1);
}

static void reply_unavailable(int fd)
{
    sl_reply(fd, "text/plain", "Unavailable route", 17);
}

static int path_is(const char *path, const char *literal)
{
    return strcmp(path, literal) == 0;
}

static int path_starts(const char *path, const char *prefix)
{
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static int parse_rtp_port(const char *path, int *port)
{
    int value = 0, used = 0;
    if (sscanf(path, "/rtp/video?port=%d%n", &value, &used) != 1) return 0;
    if (used <= 0 || path[used] != '\0') return 0;
    if (value < 1024 || value > 65535) return 0;
    *port = value;
    return 1;
}

static int streaming_route(const char *path, int rtp_port)
{
    return path_is(path, "/frames") || path_is(path, "/audio") || rtp_port != 0;
}

static void dispatch(int fd, const char *method, const char *path, int rtp_port)
{
    if (path_is(method, "GET")) {
        if (path_is(path, "/")) {
            sl_reply(fd, "text/html; charset=utf-8", SL_DASHBOARD_HTML, SL_DASHBOARD_HTML_len);
            return;
        }
        if (path_is(path, "/banner.png")) {
            sl_reply(fd, "image/png", SL_BANNER_PNG, SL_BANNER_PNG_len);
            return;
        }
        if (path_is(path, "/config.json")) { sl_http_configuration(fd); return; }
        if (path_is(path, "/status.json"))  { sl_http_status(fd); return; }
        if (path_is(path, "/frames"))       { sl_http_stream(fd); return; }
        if (path_is(path, "/audio"))        { sl_http_audio(fd); return; }
        if (rtp_port)                       { sl_http_rtp(fd, rtp_port); return; }
        if (path_is(path, "/recordings"))   { sl_http_recordings(fd); return; }
        if (path_starts(path, "/recordings/")) {
            sl_http_download(fd, path + strlen("/recordings/"));
            return;
        }
        reply_unavailable(fd);
        return;
    }

    if (path_is(method, "POST")) {
        if (path_is(path, "/record/start")) {
            pthread_mutex_lock(&g.lock);
            if (g.profile_pending) {
                pthread_mutex_unlock(&g.lock);
                sl_profile_reply(fd, 409, "Wait for the resolution change before recording");
                return;
            }
            if (g.recording == NULL && !g.record_requested) {
                g.keyframe_request = 1;
                g.record_requested = 1;
                g.rec_error = 0;
                g.rec_frames = 0;
                g.rec_bytes = 0;
            }
            pthread_mutex_unlock(&g.lock);
            sl_reply(fd, "application/json", "{}", 2);
            return;
        }
        if (path_is(path, "/record/stop")) {
            pthread_mutex_lock(&g.lock);
            g.record_requested = 0;
            g.rec_rewrite_headers = 0;
            if (g.recording) {
                fclose(g.recording);
                g.recording = NULL;
            }
            if (g.rec_started_ms) {
                g.rec_seconds = (double)(sl_now_ms() - g.rec_started_ms) / 1000.0;
                g.rec_started_ms = 0;
            }
            pthread_mutex_unlock(&g.lock);
            sl_reply(fd, "application/json", "{}", 2);
            return;
        }
        if (path_is(path, "/video/keyframe")) {
            pthread_mutex_lock(&g.lock);
            g.keyframe_request = 1;
            pthread_mutex_unlock(&g.lock);
            sl_reply(fd, "application/json", "{}", 2);
            return;
        }
        if (path_starts(path, "/capture/profile")) {
            sl_http_select_profile(fd, path);
            return;
        }
        if (path_is(path, "/capture/restart")) {
            pthread_mutex_lock(&g.lock);
            g.restart_request = 1;
            pthread_mutex_unlock(&g.lock);
            sl_reply(fd, "application/json", "{}", 2);
            return;
        }
        if (path_is(path, "/stop")) {
            sl_reply(fd, "application/json", "{}", 2);
            sl_server_stop();
            return;
        }
        reply_unavailable(fd);
        return;
    }

    reply_unavailable(fd);
}

static void *client_thread(void *arg)
{
    int *pair = (int *)arg;
    int fd = pair[0];
    int slot = pair[1];
    free(pair);

    configure_client_socket(fd);

    char request[4096];
    request[0] = 0;   /* a peer that closes before sending anything leaves it empty */
    size_t used = 0;
    int streaming = 0;
    uint64_t deadline = sl_now_ms() + 5000;

    for (;;) {
        if (used >= 4095) break;
        if (sl_now_ms() > deadline) break;
#ifdef _WIN32
        int n = recv((SOCKET)fd, request + used, (int)(4095 - used), 0);
#else
        ssize_t n = recv(fd, request + used, 4095 - used, 0);
#endif
        if (n <= 0) {
#ifdef _WIN32
            if (n < 0 && WSAGetLastError() == WSAEINTR) continue;
            if (n < 0 && (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAETIMEDOUT)) continue;
#else
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
#endif
            break;
        }
        used += (size_t)n;
        request[used] = 0;
        if (strstr(request, "\r\n\r\n")) break;
    }

    if (!strstr(request, "\r\n\r\n")) goto done;

    char method[12];
    char path[256];
    if (sscanf(request, "%11s %255s", method, path) != 2) goto done;

    int rtp_port = 0;
    parse_rtp_port(path, &rtp_port);

    pthread_mutex_lock(&g.lock);
    if (streaming_route(path, rtp_port)) {
        if (g.streaming_count >= SL_MAX_STREAMING) {
            pthread_mutex_unlock(&g.lock);
            reply_busy(fd);
            goto done;
        }
        g.streaming_count++;
        streaming = 1;
    }
    pthread_mutex_unlock(&g.lock);

    dispatch(fd, method, path, rtp_port);

done:
    pthread_mutex_lock(&g.lock);
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
    g.slot_fd[slot] = -1;
    g.client_count--;
    if (streaming) g.streaming_count--;
    g.slot_used[slot] = 0;
    pthread_mutex_unlock(&g.lock);
    return NULL;
}

static void *accept_thread(void *arg)
{
    (void)arg;

    for (;;) {
        pthread_mutex_lock(&g.lock);
        int running = g.running;
        int listen_fd = g.listen_fd;
        pthread_mutex_unlock(&g.lock);
        if (!running) break;

#ifdef _WIN32
        SOCKET client = accept((SOCKET)listen_fd, NULL, NULL);
        if (client == INVALID_SOCKET) {
            if (!g.running) break;
            continue;
        }
#else
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            if (!g.running) break;
            if (errno == EINTR) continue;
            continue;
        }
#endif

        pthread_mutex_lock(&g.lock);
        int slot = -1;
        for (int i = 0; i < SL_MAX_CLIENTS; i++) {
            if (!g.slot_used[i]) { slot = i; break; }
        }

        if (slot >= 0 && g.running) {
            if (g.thread_active[slot]) {
                pthread_t old = g.threads[slot];
                g.thread_active[slot] = 0;
                pthread_mutex_unlock(&g.lock);
                pthread_join(old, NULL);
                pthread_mutex_lock(&g.lock);
            }
            g.slot_used[slot] = 1;
            g.slot_fd[slot] = (int)client;
            g.client_count++;
            int *pair = malloc(sizeof(int) * 2);
            if (pair) {
                pair[0] = (int)client;
                pair[1] = slot;
                pthread_t thread;
                if (pthread_create(&thread, NULL, client_thread, pair) == 0) {
                    g.threads[slot] = thread;
                    g.thread_active[slot] = 1;
                    pthread_mutex_unlock(&g.lock);
                    continue;
                }
                free(pair);
            }
            /* Thread creation failed: undo the slot. */
            g.slot_used[slot] = 0;
            g.slot_fd[slot] = -1;
            g.client_count--;
            pthread_mutex_unlock(&g.lock);
            sl_close(client);
        } else {
            pthread_mutex_unlock(&g.lock);
            sl_close(client);
        }
    }

    pthread_mutex_lock(&g.lock);
    int fd = g.listen_fd;
    g.listen_fd = -1;
    pthread_mutex_unlock(&g.lock);
    if (fd >= 0) sl_close(fd);
    return NULL;
}

int sl_server_running(void)
{
    return g.running;
}

int sl_server_start(uint16_t port, uint32_t bitrate, uint16_t height, uint32_t fps,
                     const uint8_t *sps, size_t sps_len,
                     const uint8_t *pps, size_t pps_len)
{
    if (fps != 60 && fps != 30) return -1;
    if (port < 1) return -1;
    if (sps_len == 0 || sps_len > 128 || pps_len == 0 || pps_len > 128) return -1;

    pthread_mutex_lock(&g.lock);
    int already = g.running;
    pthread_mutex_unlock(&g.lock);
    if (already) {
        /* Reconfiguration path: swap the description and wake viewers. */
        pthread_mutex_lock(&g.lock);
        memcpy(g.sps, sps, sps_len); g.sps_len = sps_len;
        memcpy(g.pps, pps, pps_len); g.pps_len = pps_len;
        g.height = height;
        g.width = (uint16_t)((uint32_t)height * 16 / 9);
        g.fps = fps;
        g.bitrate = bitrate;
        g.started_ms = sl_now_ms();
        g.profile_pending = 0;
        g.profile_error = 0;
        g.discontinuity = 1;
        g.video_generation++;
        pthread_cond_broadcast(&g.cond);
        pthread_mutex_unlock(&g.lock);
        return 0;
    }

#ifdef _WIN32
    WSADATA wsa;
    static int wsa_ready = 0;
    if (!wsa_ready) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        wsa_ready = 1;
    }
#endif

    SL_SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (fd == INVALID_SOCKET) return -1;
#else
    if (fd < 0) return -1;
#endif

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        sl_close(fd);
        return -1;
    }

    pthread_mutex_lock(&g.lock);
    g.running = 1;
    g.listen_fd = (int)fd;
    g.started_ms = sl_now_ms();
    g.height = height;
    g.width = (uint16_t)((uint32_t)height * 16 / 9);
    g.fps = fps;
    g.bitrate = bitrate;
    snprintf(g.platform, sizeof(g.platform), "PS5");
    memcpy(g.sps, sps, sps_len); g.sps_len = sps_len;
    memcpy(g.pps, pps, pps_len); g.pps_len = pps_len;
    g.profile_pending = 0;
    g.profile_error = 0;
    g.video_generation++;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);

    pthread_t thread;
    if (pthread_create(&thread, NULL, accept_thread, NULL) != 0) {
        pthread_mutex_lock(&g.lock);
        g.running = 0;
        pthread_mutex_unlock(&g.lock);
        sl_close(fd);
        return -1;
    }
    /* The accept thread is joined in sl_server_wait(). */
    g.accept_thread = thread;
    g.accept_active = 1;
    sl_notify(0, "Glint ready on port %u (%up%u). Connect from the PC app.", port, height, fps);
    return 0;
}

void sl_server_stop(void)
{
    pthread_mutex_lock(&g.lock);
    g.running = 0;
    g.record_requested = 0;
    g.rec_rewrite_headers = 0;
    if (g.recording) {
        fclose(g.recording);
        g.recording = NULL;
    }
    if (g.listen_fd >= 0) {
#ifdef _WIN32
        shutdown((SOCKET)g.listen_fd, 2);
#else
        shutdown(g.listen_fd, SHUT_RDWR);
#endif
    }
    for (int i = 0; i < SL_MAX_CLIENTS; i++) {
        if (g.slot_used[i] && g.slot_fd[i] >= 0) {
#ifdef _WIN32
            shutdown((SOCKET)g.slot_fd[i], 2);
#else
            shutdown(g.slot_fd[i], SHUT_RDWR);
#endif
        }
    }
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);
}

void sl_server_wait(void)
{
    pthread_mutex_lock(&g.lock);
    int accept_active = g.accept_active;
    pthread_t accept_handle = g.accept_thread;
    g.accept_active = 0;
    pthread_mutex_unlock(&g.lock);
    if (accept_active) pthread_join(accept_handle, NULL);

    /* Join every client thread exactly once. */
    for (int i = 0; i < SL_MAX_CLIENTS; i++) {
        pthread_mutex_lock(&g.lock);
        int active = g.thread_active[i];
        pthread_t thread = g.threads[i];
        g.thread_active[i] = 0;
        pthread_mutex_unlock(&g.lock);
        if (active) pthread_join(thread, NULL);
    }

    /* Free rings. */
    pthread_mutex_lock(&g.lock);
    for (int i = 0; i < SL_VIDEO_SLOTS; i++) {
        free(g.video[i].data);
        g.video[i].data = NULL;
        g.video[i].len = g.video[i].cap = 0;
    }
    for (int i = 0; i < SL_AUDIO_SLOTS; i++) {
        free(g.audio[i].data);
        g.audio[i].data = NULL;
        g.audio[i].len = 0;
    }
    pthread_mutex_unlock(&g.lock);
}



