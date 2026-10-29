#ifndef SL_HTTP_HANDLERS_H
#define SL_HTTP_HANDLERS_H

/* Responses produced by media_http.c. Each handler owns the request socket
 * and sends the complete HTTP response. */
void sl_http_configuration(int fd);
void sl_http_status(int fd);
void sl_http_stream(int fd, uint64_t epoch);
void sl_http_audio(int fd, uint64_t epoch);
void sl_http_recordings(int fd);
void sl_http_download(int fd, const char *name);
void sl_http_rtp(int fd, int port, uint64_t epoch);
void sl_http_select_profile(int fd, const char *path);

/* Shared helpers (server.c). */
void sl_send_all(int fd, const void *data, size_t len);
void sl_reply(int fd, const char *type, const void *body, size_t len);
void sl_profile_reply(int fd, int code, const char *message);

#endif
