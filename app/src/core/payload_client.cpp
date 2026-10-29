#include "glint/payload_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace glint {
namespace {

std::once_flag g_curl_once;

std::string trim(const std::string& value) {
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

size_t append_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(data, size * count);
    return size * count;
}

size_t vector_callback(char* data, size_t size, size_t count, void* userdata) {
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(data);
    out->insert(out->end(), begin, begin + size * count);
    return size * count;
}

uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

struct StreamState {
    const uint8_t* unused = nullptr;
    std::vector<uint8_t> pending;
    bool failed = false;
    std::string error;
};

} // namespace

std::string json_string(const std::string& document, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t at = document.find(needle);
    if (at == std::string::npos) return "";
    at = document.find(':', at + needle.size());
    if (at == std::string::npos) return "";
    at++;
    while (at < document.size() && std::isspace(static_cast<unsigned char>(document[at]))) at++;
    if (at >= document.size() || document[at] != '"') return "";
    at++;

    std::string value;
    while (at < document.size()) {
        char c = document[at++];
        if (c == '"') break;
        if (c == '\\' && at < document.size()) {
            char escaped = document[at++];
            switch (escaped) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'u': {
                if (at + 4 <= document.size()) {
                    unsigned code = static_cast<unsigned>(
                        std::strtoul(document.substr(at, 4).c_str(), nullptr, 16));
                    at += 4;
                    if (code < 0x80) value.push_back(static_cast<char>(code));
                    else if (code < 0x800) {
                        value.push_back(static_cast<char>(0xc0 | (code >> 6)));
                        value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    } else {
                        value.push_back(static_cast<char>(0xe0 | (code >> 12)));
                        value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
                        value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
                    }
                }
                break;
            }
            default: value.push_back(escaped); break;
            }
        } else {
            value.push_back(c);
        }
    }
    return value;
}

long long json_int(const std::string& document, const std::string& key, long long fallback) {
    std::string needle = "\"" + key + "\"";
    size_t at = document.find(needle);
    if (at == std::string::npos) return fallback;
    at = document.find(':', at + needle.size());
    if (at == std::string::npos) return fallback;
    at++;
    while (at < document.size() && std::isspace(static_cast<unsigned char>(document[at]))) at++;
    char* end = nullptr;
    long long value = std::strtoll(document.c_str() + at, &end, 10);
    return end == document.c_str() + at ? fallback : value;
}

double json_double(const std::string& document, const std::string& key, double fallback) {
    std::string needle = "\"" + key + "\"";
    size_t at = document.find(needle);
    if (at == std::string::npos) return fallback;
    at = document.find(':', at + needle.size());
    if (at == std::string::npos) return fallback;
    at++;
    while (at < document.size() && std::isspace(static_cast<unsigned char>(document[at]))) at++;
    char* end = nullptr;
    double value = std::strtod(document.c_str() + at, &end);
    return end == document.c_str() + at ? fallback : value;
}

bool json_bool(const std::string& document, const std::string& key, bool fallback) {
    std::string needle = "\"" + key + "\"";
    size_t at = document.find(needle);
    if (at == std::string::npos) return fallback;
    at = document.find(':', at + needle.size());
    if (at == std::string::npos) return fallback;
    at++;
    while (at < document.size() && std::isspace(static_cast<unsigned char>(document[at]))) at++;
    if (document.compare(at, 4, "true") == 0) return true;
    if (document.compare(at, 5, "false") == 0) return false;
    return fallback;
}

std::vector<int> json_int_array(const std::string& document, const std::string& key) {
    std::vector<int> values;
    std::string needle = "\"" + key + "\"";
    size_t at = document.find(needle);
    if (at == std::string::npos) return values;
    at = document.find('[', at);
    if (at == std::string::npos) return values;
    size_t end = document.find(']', at);
    if (end == std::string::npos) return values;

    std::string body = document.substr(at + 1, end - at - 1);
    size_t pos = 0;
    while (pos < body.size()) {
        while (pos < body.size() &&
               !std::isdigit(static_cast<unsigned char>(body[pos])) && body[pos] != '-')
            pos++;
        if (pos >= body.size()) break;
        char* stop = nullptr;
        long value = std::strtol(body.c_str() + pos, &stop, 10);
        if (stop == body.c_str() + pos) break;
        values.push_back(static_cast<int>(value));
        pos = static_cast<size_t>(stop - body.c_str());
    }
    return values;
}

PayloadClient::PayloadClient(std::string base_url) : base_(std::move(base_url)) {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
}

namespace {

bool perform_get(const std::string& url, size_t (*write)(char*, size_t, size_t, void*),
                 void* userdata, long timeout_ms, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, userdata);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Glint/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        error = curl_easy_strerror(result);
        return false;
    }
    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

} // namespace

bool PayloadClient::fetch_config(StreamConfig& config, std::string& error) {
    std::string document;
    if (!perform_get(base_ + "/config.json", append_callback, &document, 5000, error))
        return false;

    config.platform = json_string(document, "platform");
    config.width = static_cast<int>(json_int(document, "width"));
    config.height = static_cast<int>(json_int(document, "height"));
    config.fps = static_cast<int>(json_int(document, "fps"));
    config.codec = json_string(document, "codec");
    config.sps = json_string(document, "sps");
    config.pps = json_string(document, "pps");
    config.resolutions = json_int_array(document, "resolutions");
    config.requested_height = static_cast<int>(json_int(document, "requested_height"));
    config.profile_changing = json_bool(document, "profile_changing");
    config.profile_error = json_bool(document, "profile_error");

    size_t audio = document.find("\"audio\"");
    config.has_audio = false;
    if (audio != std::string::npos) {
        size_t colon = document.find(':', audio);
        if (colon != std::string::npos) {
            size_t begin = document.find_first_not_of(" \t\r\n", colon + 1);
            if (begin != std::string::npos && document[begin] == '{') {
                int depth = 0;
                size_t end = begin;
                bool in_string = false;
                for (; end < document.size(); end++) {
                    char c = document[end];
                    if (in_string) {
                        if (c == '\\') end++;
                        else if (c == '"') in_string = false;
                        continue;
                    }
                    if (c == '"') in_string = true;
                    else if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        if (depth == 0) { end++; break; }
                    }
                }
                std::string block = document.substr(begin, end - begin);
                config.has_audio = true;
                config.audio_codec = json_string(block, "codec");
                config.audio_channels = static_cast<int>(json_int(block, "channels"));
                config.audio_rate = static_cast<int>(json_int(block, "rate"));
                config.audio_bits = static_cast<int>(json_int(block, "bits"));
            }
        }
    }
    return true;
}

bool PayloadClient::fetch_status(StreamStatus& status, std::string& error) {
    std::string document;
    if (!perform_get(base_ + "/status.json", append_callback, &document, 5000, error))
        return false;

    status.backend = json_string(document, "backend");
    status.width = static_cast<int>(json_int(document, "width"));
    status.height = static_cast<int>(json_int(document, "height"));
    status.frames = static_cast<unsigned long long>(json_int(document, "frames"));
    status.bytes = static_cast<unsigned long long>(json_int(document, "bytes"));
    status.fps = json_double(document, "fps");
    status.kbps = json_double(document, "kbps");
    status.clients = static_cast<int>(json_int(document, "clients"));
    status.viewers = static_cast<int>(json_int(document, "viewers"));
    status.recording = json_bool(document, "recording");
    status.recording_name = json_string(document, "recording_name");
    status.recording_frames = static_cast<unsigned long long>(json_int(document, "recording_frames"));
    status.recording_bytes = static_cast<unsigned long long>(json_int(document, "recording_bytes"));
    status.recording_seconds = json_double(document, "recording_seconds");
    status.recording_error = json_bool(document, "recording_error");
    status.audio_chunks = static_cast<unsigned long long>(json_int(document, "audio_chunks"));
    status.audio_bytes = static_cast<unsigned long long>(json_int(document, "audio_bytes"));
    status.audio_kbps = json_double(document, "audio_kbps");
    status.audio_codec = json_string(document, "audio_codec");
    return true;
}

bool PayloadClient::fetch_bytes(const std::string& path, std::vector<uint8_t>& out,
                                std::string& error, long timeout_ms) {
    return perform_get(base_ + path, vector_callback, &out, timeout_ms, error);
}

bool PayloadClient::post(const std::string& path, std::string* body, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, (base_ + path).c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Glint/1.0");

    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (result != CURLE_OK) {
        error = curl_easy_strerror(result);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status);
        if (!response.empty()) error += ": " + response;
        return false;
    }
    if (body) *body = std::move(response);
    return true;
}

bool PayloadClient::stream_frames(const FrameCallback& callback, std::atomic_bool& stop,
                                  std::string& error, int idle_timeout_ms) {
    StreamState state;

    /* The parsing loop needs the callback, so it lives outside the plain C
     * callback thunk above. */
    struct Context {
        const FrameCallback* callback;
        StreamState* state;
        std::atomic_bool* stop;
    } context{&callback, &state, &stop};

    auto thunk = [](char* data, size_t size, size_t count, void* userdata) -> size_t {
        auto* ctx = static_cast<Context*>(userdata);
        size_t total = size * count;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        ctx->state->pending.insert(ctx->state->pending.end(), bytes, bytes + total);

        size_t offset = 0;
        while (ctx->state->pending.size() - offset >= 8) {
            if (ctx->stop->load()) return 0;
            uint32_t frame_size = read_be32(ctx->state->pending.data() + offset);
            uint32_t pts = read_be32(ctx->state->pending.data() + offset + 4);
            if (frame_size == 0 || frame_size > 2u * 1024u * 1024u) {
                ctx->state->failed = true;
                ctx->state->error = "invalid frame size";
                return 0;
            }
            if (ctx->state->pending.size() - offset < 8u + frame_size) break;
            const uint8_t* frame = ctx->state->pending.data() + offset + 8;
            if (!(*ctx->callback)(frame, frame_size, pts)) return 0;
            offset += 8u + frame_size;
        }
        if (offset) ctx->state->pending.erase(ctx->state->pending.begin(),
                                              ctx->state->pending.begin() + offset);
        return total;
    };

    // Without this a stalled stream only notices `stop` after the idle timeout.
    auto progress = [](void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        return static_cast<Context*>(userdata)->stop->load() ? 1 : 0;
    };

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }
    // One viewer at a time: the app only connects when asked to, so it takes
    // the stream from whoever has it (docs/API.md).
    curl_easy_setopt(curl, CURLOPT_URL, (base_ + "/frames?take=1").c_str());
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)(idle_timeout_ms / 1000));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +thunk);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Glint/1.0");

    CURLcode result = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (result == CURLE_HTTP_RETURNED_ERROR && http_status == 409) {
        error = kTakenOver;
        return false;
    }
    if (state.failed) {
        error = state.error;
        return false;
    }
    if (result != CURLE_OK && !stop.load()) {
        error = curl_easy_strerror(result);
        return false;
    }
    return true;
}

bool PayloadClient::stream_audio(const AudioCallback& callback, std::atomic_bool& stop,
                                 std::string& error, int idle_timeout_ms) {
    StreamState state;
    struct Context {
        const AudioCallback* callback;
        StreamState* state;
        std::atomic_bool* stop;
    } context{&callback, &state, &stop};

    auto thunk = [](char* data, size_t size, size_t count, void* userdata) -> size_t {
        auto* ctx = static_cast<Context*>(userdata);
        size_t total = size * count;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        ctx->state->pending.insert(ctx->state->pending.end(), bytes, bytes + total);

        size_t offset = 0;
        while (ctx->state->pending.size() - offset >= 8) {
            if (ctx->stop->load()) return 0;
            uint32_t packet_size = read_be32(ctx->state->pending.data() + offset);
            uint32_t pts = read_be32(ctx->state->pending.data() + offset + 4);
            if (packet_size == 0 || packet_size > 65536u) {
                ctx->state->failed = true;
                ctx->state->error = "invalid audio packet";
                return 0;
            }
            if (ctx->state->pending.size() - offset < 8u + packet_size) break;
            const uint8_t* packet = ctx->state->pending.data() + offset + 8;
            if (!(*ctx->callback)(packet, packet_size, pts)) return 0;
            offset += 8u + packet_size;
        }
        if (offset) ctx->state->pending.erase(ctx->state->pending.begin(),
                                              ctx->state->pending.begin() + offset);
        return total;
    };

    auto progress = [](void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        return static_cast<Context*>(userdata)->stop->load() ? 1 : 0;
    };

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, (base_ + "/audio").c_str());   // never takes: it reconnects on its own
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, (long)(idle_timeout_ms / 1000));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +thunk);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Glint/1.0");

    CURLcode result = curl_easy_perform(curl);
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_cleanup(curl);

    if (result == CURLE_HTTP_RETURNED_ERROR && http_status == 409) {
        error = kTakenOver;
        return false;
    }
    if (state.failed) {
        error = state.error;
        return false;
    }
    if (result != CURLE_OK && !stop.load()) {
        error = curl_easy_strerror(result);
        return false;
    }
    return true;
}

} // namespace glint

