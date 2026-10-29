#include "glint.h"
#include "http_handlers.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

static const char STREAM_HEAD[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n\r\n";

static void close_socket_fd(int fd)
{
#ifdef _WIN32
    closesocket((SOCKET)fd);
#else
    close(fd);
#endif
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int json_name_ok(const char *name)
{
    size_t len = strlen(name);
    if (len < 6 || len > 96) return 0;
    if (name[0] == '.') return 0;
    if (strcmp(name + len - 5, ".h264") != 0) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.'))
            return 0;
    }
    return strstr(name, "..") == NULL;
}

/* Non-blocking peer check. Returns 0 when the viewer closed the socket. */
static int client_alive(int fd)
{
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET)fd, &set);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(0, &set, NULL, NULL, &tv) <= 0) return 1;
    char c;
    int n = recv((SOCKET)fd, &c, 1, MSG_PEEK);
    return n != 0;
#else
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return 0;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
    return n > 0;
#endif
}

/* Called with g.lock held: wait up to one second for new media on the ring
 * identified by `audio`, or for the viewer to disconnect. Returns 0 when the
 * caller should stop (server stopping, stream generation bumped, another
 * viewer took over, or peer closed), non-zero to keep streaming. */
static int wait_media(int fd, uint64_t generation, int audio, uint64_t epoch)
{
    if (!g.running) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    pthread_cond_timedwait(&g.cond, &g.lock, &ts);

    if (!g.running) return 0;
    if (generation != (audio ? g.audio_generation : g.video_generation)) return 0;
    if (epoch != g.viewer_epoch) return 0;
    return client_alive(fd);
}

/* ------------------------------------------------------------------ */
/* /config.json                                                        */
/* ------------------------------------------------------------------ */
static void hex_encode(char *out, const uint8_t *data, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    out[len * 2] = 0;
}

void sl_http_configuration(int fd)
{
    char json[1024];
    char sps[257], pps[257];
    char audio[160];
    int requested;

    pthread_mutex_lock(&g.lock);
    hex_encode(sps, g.sps, g.sps_len);
    hex_encode(pps, g.pps, g.pps_len);

    if (g.audio_none || g.audio_codec[0] == 0 || strcmp(g.audio_codec, "none") == 0) {
        snprintf(audio, sizeof(audio), "null");
    } else {
        snprintf(audio, sizeof(audio),
                 "{\"codec\":\"%s\",\"channels\":%d,\"rate\":%d,\"bits\":32}",
                 g.audio_codec, g.audio_channels, g.audio_rate);
    }

    requested = g.profile_pending ? g.requested_height
                                  : (g.requested_height ? g.requested_height : g.height);

    int n = snprintf(json, sizeof(json),
        "{\"platform\":\"%s\",\"width\":%d,\"height\":%d,\"fps\":%d,"
        "\"codec\":\"avc1.%02x%02x%02x\",\"sps\":\"%s\",\"pps\":\"%s\","
        "\"audio\":%s,\"resolutions\":[1080],"
        "\"requested_height\":%d,\"profile_changing\":%s,\"profile_error\":%s}",
        g.platform, g.width, g.height, g.fps,
        g.sps_len > 1 ? g.sps[1] : 0, g.sps_len > 2 ? g.sps[2] : 0,
        g.sps_len > 3 ? g.sps[3] : 0,
        sps, pps, audio, requested,
        g.profile_pending ? "true" : "false",
        g.profile_error ? "true" : "false");
    pthread_mutex_unlock(&g.lock);

    if (n > 0 && (size_t)n < sizeof(json))
        sl_reply(fd, "application/json", json, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* /status.json                                                        */
/* ------------------------------------------------------------------ */
void sl_http_status(int fd)
{
    char json[768];
    char name[100];

    pthread_mutex_lock(&g.lock);
    uint64_t now = sl_now_ms();
    double elapsed = g.started_ms ? (double)(now - g.started_ms) / 1000.0 : 0.0;
    double fps = elapsed > 0.0 ? (double)g.video_frames / elapsed : 0.0;
    double kbps = elapsed > 0.0 ? (double)g.video_bytes * 8.0 / elapsed / 1000.0 : 0.0;

    int recording = g.recording != NULL;
    double seconds = recording && g.rec_started_ms
        ? (double)(now - g.rec_started_ms) / 1000.0
        : g.rec_seconds;

    snprintf(name, sizeof(name), "%s", recording || g.recording_name[0] ? g.recording_name : "");

    double audio_elapsed = g.audio_started_ms ? (double)(now - g.audio_started_ms) / 1000.0 : 0.0;
    double audio_kbps = audio_elapsed > 0.0
        ? (double)g.audio_bytes * 8.0 / audio_elapsed / 1000.0 : 0.0;

    int n = snprintf(json, sizeof(json),
        "{\"version\":\"" SL_VERSION "\",\"backend\":\"h264\",\"width\":%d,\"height\":%d,\"frames\":%llu,\"bytes\":%llu,"
        "\"fps\":%.1f,\"kbps\":%.1f,\"clients\":%d,\"viewers\":%d,\"bitrate\":%u,\"recording\":%s,"
        "\"recording_name\":\"%s\",\"recording_frames\":%llu,\"recording_bytes\":%llu,"
        "\"recording_seconds\":%.1f,\"recording_error\":%s,"
        "\"audio_chunks\":%llu,\"audio_bytes\":%llu,\"audio_kbps\":%.1f,"
        "\"audio_codec\":\"%s\"}",
        g.width, g.height,
        (unsigned long long)g.video_frames, (unsigned long long)g.video_bytes,
        fps, kbps, g.client_count, g.viewer_streams > 0 ? 1 : 0,
        (unsigned)sl_opts.bitrate, recording ? "true" : "false",
        name,
        (unsigned long long)g.rec_frames, (unsigned long long)g.rec_bytes,
        seconds, g.rec_error ? "true" : "false",
        (unsigned long long)g.audio_chunks, (unsigned long long)g.audio_bytes,
        audio_kbps, g.audio_codec);
    pthread_mutex_unlock(&g.lock);

    if (n > 0 && (size_t)n < sizeof(json))
        sl_reply(fd, "application/json", json, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* /frames                                                             */
/* ------------------------------------------------------------------ */
void sl_http_stream(int fd, uint64_t epoch)
{
    sl_send_all(fd, STREAM_HEAD, sizeof(STREAM_HEAD) - 1);

    pthread_mutex_lock(&g.lock);
    if (!g.running) {
        pthread_mutex_unlock(&g.lock);
        return;
    }

    uint64_t generation = g.video_generation;
    uint64_t cursor = g.video_seq + 1;
    /* One keyframe to start on, and another after a skip below. No periodic
     * ones: TCP loses nothing, and on Wi-Fi each 250-350 KB IDR is a latency
     * spike. A client that wants them (the PC recorder) asks /video/keyframe. */
    g.keyframe_request = 1;

    uint8_t *packet = NULL;
    size_t packet_cap = 0;

    while (g.running && generation == g.video_generation && epoch == g.viewer_epoch) {
        if (g.video_seq < cursor) {
            if (!wait_media(fd, generation, 0, epoch)) break;
            continue;
        }

        /* Bound the backlog: skip to the newest random access point. */
        if (g.video_seq - cursor > 6) {
            g.keyframe_request = 1;
            uint64_t newest = cursor;
            for (uint64_t s = g.video_seq; s > cursor; s--) {
                sl_video_slot *candidate = &g.video[s & (SL_VIDEO_SLOTS - 1)];
                if (candidate->seq == s && candidate->key) { newest = s; break; }
            }
            cursor = (newest > cursor) ? newest : g.video_seq + 1;
            if (cursor > g.video_seq) continue;
        }

        sl_video_slot *slot = &g.video[cursor & (SL_VIDEO_SLOTS - 1)];
        if (slot->seq != cursor || slot->data == NULL) {
            cursor++;
            continue;
        }

        size_t len = slot->len;
        uint32_t pts = slot->pts;
        if (packet_cap < len + 8) {
            size_t cap = 0x4000;
            while (cap < len + 8) cap *= 2;
            uint8_t *grown = realloc(packet, cap);
            if (!grown) break;
            packet = grown;
            packet_cap = cap;
        }
        wr_be32(packet, (uint32_t)len);
        wr_be32(packet + 4, pts);
        memcpy(packet + 8, slot->data, len);
        cursor++;

        pthread_mutex_unlock(&g.lock);
        sl_send_all(fd, packet, len + 8);
        pthread_mutex_lock(&g.lock);
    }

    free(packet);
    pthread_mutex_unlock(&g.lock);
}

/* ------------------------------------------------------------------ */
/* /audio                                                              */
/* ------------------------------------------------------------------ */
void sl_http_audio(int fd, uint64_t epoch)
{
    sl_send_all(fd, STREAM_HEAD, sizeof(STREAM_HEAD) - 1);

    pthread_mutex_lock(&g.lock);
    if (!g.running) {
        pthread_mutex_unlock(&g.lock);
        return;
    }

    uint64_t generation = g.audio_generation;
    uint64_t cursor = g.audio_seq + 1;
    uint8_t *packet = malloc(0x10008);
    if (!packet) {
        pthread_mutex_unlock(&g.lock);
        return;
    }

    while (g.running && generation == g.audio_generation && epoch == g.viewer_epoch) {
        if (g.audio_seq < cursor) {
            if (!wait_media(fd, generation, 1, epoch)) break;
            continue;
        }
        sl_audio_slot *slot = &g.audio[cursor & (SL_AUDIO_SLOTS - 1)];
        if (slot->seq != cursor || slot->data == NULL) {
            cursor++;
            continue;
        }
        size_t len = slot->len;
        wr_be32(packet, (uint32_t)len);
        wr_be32(packet + 4, slot->pts);
        memcpy(packet + 8, slot->data, len);
        cursor++;

        pthread_mutex_unlock(&g.lock);
        sl_send_all(fd, packet, len + 8);
        pthread_mutex_lock(&g.lock);
    }

    free(packet);
    pthread_mutex_unlock(&g.lock);
}

/* ------------------------------------------------------------------ */
/* /rtp/video?port=N                                                   */
/* ------------------------------------------------------------------ */
static const char RTP_HEAD[] =
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\n"
    "Connection: close\r\n\r\nRTP\n";

static int rtp_send_packet(
#ifdef _WIN32
    SOCKET udp,
#else
    int udp,
#endif
    const struct sockaddr_in *peer, uint16_t seq, uint32_t timestamp,
    uint32_t ssrc, int marker, const uint8_t *payload, size_t len)
{
    uint8_t packet[1500];
    if (len > sizeof(packet) - 12) return -1;
    packet[0] = 0x80;
    packet[1] = (uint8_t)((marker ? 0x80 : 0) | 96);
    packet[2] = (uint8_t)(seq >> 8);
    packet[3] = (uint8_t)seq;
    packet[4] = (uint8_t)(timestamp >> 24);
    packet[5] = (uint8_t)(timestamp >> 16);
    packet[6] = (uint8_t)(timestamp >> 8);
    packet[7] = (uint8_t)timestamp;
    packet[8] = (uint8_t)(ssrc >> 24);
    packet[9] = (uint8_t)(ssrc >> 16);
    packet[10] = (uint8_t)(ssrc >> 8);
    packet[11] = (uint8_t)ssrc;
    memcpy(packet + 12, payload, len);
#ifdef _WIN32
    return sendto(udp, (const char *)packet, (int)(len + 12), 0,
                  (const struct sockaddr *)peer, sizeof(*peer)) == (int)(len + 12) ? 0 : -1;
#else
    return sendto(udp, packet, len + 12, 0,
                  (const struct sockaddr *)peer, sizeof(*peer)) == (ssize_t)(len + 12) ? 0 : -1;
#endif
}

static size_t annexb_nals(const uint8_t *data, size_t len,
                          const uint8_t **nals, size_t *sizes, size_t max)
{
    size_t count = 0;
    size_t i = 0;
    size_t start = (size_t)-1;
    size_t header = 0;

    while (i + 2 < len) {
        size_t sc = 0;
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) sc = 3;
            else if (i + 3 < len && data[i + 2] == 0 && data[i + 3] == 1) sc = 4;
        }
        if (sc) {
            if (start != (size_t)-1 && i > start + header) {
                if (count < max) {
                    nals[count] = data + start + header;
                    sizes[count] = i - (start + header);
                    count++;
                }
            }
            start = i;
            header = sc;
            i += sc;
        } else {
            i++;
        }
    }
    if (start != (size_t)-1 && len > start + header) {
        if (count < max) {
            nals[count] = data + start + header;
            sizes[count] = len - (start + header);
            count++;
        }
    }
    return count;
}

static uint32_t rtp_ssrc(int fd, uint64_t started)
{
    uint32_t value = (uint32_t)fd * 2654435761u ^ (uint32_t)started;
    return value ? value : 1;
}

void sl_http_rtp(int fd, int port, uint64_t epoch)
{
    struct sockaddr_in peer;
#ifdef _WIN32
    int peer_len = sizeof(peer);
    if (getpeername((SOCKET)fd, (struct sockaddr *)&peer, &peer_len) != 0) return;
    SOCKET udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp == INVALID_SOCKET) return;
#else
    socklen_t peer_len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &peer_len) != 0) return;
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp < 0) return;
#endif
    peer.sin_port = htons((uint16_t)port);

    sl_send_all(fd, RTP_HEAD, sizeof(RTP_HEAD) - 1);

    pthread_mutex_lock(&g.lock);
    uint64_t generation = g.video_generation;
    uint64_t cursor = g.video_seq + 1;
    uint32_t ssrc = rtp_ssrc(fd, g.started_ms);
    uint16_t sequence = (uint16_t)(ssrc & 0xffff);
    g.keyframe_request = 1;

    uint8_t *frame = NULL;
    size_t frame_cap = 0;
    const uint8_t *nals[256];
    size_t sizes[256];

    while (g.running && generation == g.video_generation && epoch == g.viewer_epoch) {
        if (g.video_seq < cursor) {
            if (!wait_media(fd, generation, 0, epoch)) break;
            continue;
        }
        sl_video_slot *slot = &g.video[cursor & (SL_VIDEO_SLOTS - 1)];
        if (slot->seq != cursor || slot->data == NULL) {
            cursor++;
            continue;
        }

        size_t prefix = slot->key ? g.sps_len + g.pps_len + 8 : 0;
        size_t need = slot->len + prefix + 8;
        if (frame_cap < need) {
            size_t cap = 0x8000;
            while (cap < need) cap *= 2;
            uint8_t *grown = realloc(frame, cap);
            if (!grown) break;
            frame = grown;
            frame_cap = cap;
        }

        size_t offset = 0;
        if (slot->key) {
            memcpy(frame + offset, "\x00\x00\x00\x01", 4); offset += 4;
            memcpy(frame + offset, g.sps, g.sps_len); offset += g.sps_len;
            memcpy(frame + offset, "\x00\x00\x00\x01", 4); offset += 4;
            memcpy(frame + offset, g.pps, g.pps_len); offset += g.pps_len;
        }
        memcpy(frame + offset, slot->data, slot->len);
        size_t total = offset + slot->len;
        uint32_t pts = slot->pts;
        cursor++;

        pthread_mutex_unlock(&g.lock);

        size_t count = annexb_nals(frame, total, nals, sizes, 256);
        for (size_t n = 0; n < count; n++) {
            const uint8_t *nal = nals[n];
            size_t len = sizes[n];
            int last = (n + 1 == count);
            if (len == 0 || (nal[0] & 0x80)) continue;

            const size_t mtu = 1400;
            if (len <= mtu) {
                rtp_send_packet(udp, &peer, sequence++, pts, ssrc, last, nal, len);
            } else {
                uint8_t indicator = (uint8_t)((nal[0] & 0xe0) | 28);
                uint8_t type = (uint8_t)(nal[0] & 0x1f);
                size_t pos = 1;
                int first = 1;
                while (pos < len) {
                    size_t chunk = len - pos;
                    if (chunk > mtu - 2) chunk = mtu - 2;
                    int end = (pos + chunk >= len);
                    uint8_t fragment[mtu];
                    fragment[0] = indicator;
                    fragment[1] = (uint8_t)((first ? 0x80 : 0) | (end ? 0x40 : 0) | type);
                    memcpy(fragment + 2, nal + pos, chunk);
                    rtp_send_packet(udp, &peer, sequence++, pts, ssrc,
                                    last && end, fragment, chunk + 2);
                    pos += chunk;
                    first = 0;
                }
            }
        }

        pthread_mutex_lock(&g.lock);
    }

    free(frame);
    pthread_mutex_unlock(&g.lock);
    close_socket_fd((int)udp);
}

/* ------------------------------------------------------------------ */
/* /recordings and /recordings/<name>                                  */
/* ------------------------------------------------------------------ */
void sl_http_recordings(int fd)
{
    char json[16384];
    size_t used = 0;
    json[used++] = '[';

    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/recordings", sl_data_dir());
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && used < 0x3f00) {
            const char *name = entry->d_name;
            if (!json_name_ok(name)) continue;
            char path[768];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%s", dir_path, name);
            if (stat(path, &st) != 0 || !(st.st_mode & S_IFREG)) continue;
            int n = snprintf(json + used, 0x4000 - used,
                             "%s{\"name\":\"%s\",\"bytes\":%llu}",
                             used > 1 ? "," : "", name,
                             (unsigned long long)st.st_size);
            if (n > 0) used += (size_t)n;
        }
        closedir(dir);
    }
    json[used++] = ']';
    sl_reply(fd, "application/json", json, used);
}

void sl_http_download(int fd, const char *name)
{
    if (!json_name_ok(name)) return;

    char path[768];
    snprintf(path, sizeof(path), "%s/recordings/%s", sl_data_dir(), name);

    FILE *file = fopen(path, "rb");
    if (!file) return;

    struct stat st;
    if (fstat(fileno(file), &st) != 0) {
        fclose(file);
        return;
    }

    char head[392];
    int n = snprintf(head, sizeof(head),
        "HTTP/1.1 200 OK\r\nContent-Type: video/h264\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %llu\r\nConnection: close\r\n\r\n",
        name, (unsigned long long)st.st_size);
    if (n > 0) sl_send_all(fd, head, (size_t)n);

    char buffer[16384];
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        sl_send_all(fd, buffer, got);
    }
    fclose(file);
}

/* ------------------------------------------------------------------ */
/* POST /capture/profile?height=N                                      */
/* ------------------------------------------------------------------ */
void sl_http_select_profile(int fd, const char *path)
{
    int height = 0;
    int used = 0;
    if (sscanf(path, "/capture/profile?height=%d%n", &height, &used) != 1 ||
        used <= 0 || path[used] != 0) {
        sl_profile_reply(fd, 400, "Unsupported resolution");
        return;
    }
    if (height != 1080) {   /* 1080p60 is the only profile shipped */
        sl_profile_reply(fd, 400, "Unsupported resolution");
        return;
    }

    pthread_mutex_lock(&g.lock);
    if (g.recording) {
        pthread_mutex_unlock(&g.lock);
        sl_profile_reply(fd, 409, "Stop recording before changing resolution");
        return;
    }
    if (g.profile_pending) {
        pthread_mutex_unlock(&g.lock);
        sl_profile_reply(fd, 409, "A resolution change is already in progress");
        return;
    }
    if (height == g.height) {
        pthread_mutex_unlock(&g.lock);
        sl_profile_reply(fd, 200, "Resolution unchanged");
        return;
    }
    g.requested_height = height;
    g.profile_pending = 1;
    g.profile_error = 0;
    pthread_cond_broadcast(&g.cond);
    pthread_mutex_unlock(&g.lock);

    sl_profile_reply(fd, 200, "Resolution change requested");
}



