#include "glint/payload_client.h"

#include <curl/curl.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace glint {

const char* const kTakenOver = "Another viewer has the stream (HTTP 409).";

namespace {

using Clock = std::chrono::steady_clock;

void curl_setup()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// --- tiny JSON reader ----------------------------------------------------------

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0.0;
    std::string text;   // string value, or the number as written
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* get(const char* key) const
    {
        for (const auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    bool parse(Json& out)
    {
        if (!value(out, 0)) return false;
        ws();
        return pos_ == s_.size();
    }

private:
    void ws()
    {
        while (pos_ < s_.size() && std::strchr(" \t\r\n", s_[pos_])) pos_++;
    }
    bool lit(const char* word)
    {
        size_t n = std::strlen(word);
        if (s_.compare(pos_, n, word) != 0) return false;
        pos_ += n;
        return true;
    }
    bool string(std::string& out)
    {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        pos_++;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) return false;
            char e = s_[pos_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (pos_ + 4 > s_.size()) return false;
                unsigned cp = (unsigned)std::strtoul(s_.substr(pos_, 4).c_str(), nullptr, 16);
                pos_ += 4;
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += e; break;
            }
        }
        return false;
    }
    bool value(Json& v, int depth)
    {
        if (depth > 32) return false;
        ws();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '{') {
            v.type = Json::Object;
            pos_++;
            ws();
            if (pos_ < s_.size() && s_[pos_] == '}') return pos_++, true;
            for (;;) {
                ws();
                std::pair<std::string, Json> f;
                if (!string(f.first)) return false;
                ws();
                if (pos_ >= s_.size() || s_[pos_++] != ':') return false;
                if (!value(f.second, depth + 1)) return false;
                v.fields.push_back(std::move(f));
                ws();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { pos_++; continue; }
                if (s_[pos_] == '}') return pos_++, true;
                return false;
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            pos_++;
            ws();
            if (pos_ < s_.size() && s_[pos_] == ']') return pos_++, true;
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
                ws();
                if (pos_ >= s_.size()) return false;
                if (s_[pos_] == ',') { pos_++; continue; }
                if (s_[pos_] == ']') return pos_++, true;
                return false;
            }
        }
        if (c == '"') {
            v.type = Json::String;
            return string(v.text);
        }
        if (lit("true")) { v.type = Json::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Json::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Json::Null; return true; }
        size_t start = pos_;
        while (pos_ < s_.size() && std::strchr("+-0123456789.eE", s_[pos_])) pos_++;
        if (pos_ == start) return false;
        v.type = Json::Number;
        v.text = s_.substr(start, pos_ - start);
        char* end = nullptr;
        v.num = std::strtod(v.text.c_str(), &end);
        return end && *end == 0;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

void take(const Json& o, const char* key, std::string& out)
{
    const Json* v = o.get(key);
    if (v && v->type == Json::String) out = v->text;
}
void take(const Json& o, const char* key, int& out)
{
    const Json* v = o.get(key);
    if (v && v->type == Json::Number && std::isfinite(v->num)) out = (int)v->num;
}
void take(const Json& o, const char* key, unsigned long long& out)
{
    const Json* v = o.get(key);
    if (!v || v->type != Json::Number) return;
    if (v->text.find_first_of(".eE-") == std::string::npos) out = std::strtoull(v->text.c_str(), nullptr, 10);
    else if (std::isfinite(v->num) && v->num >= 0) out = (unsigned long long)v->num;
}
void take(const Json& o, const char* key, double& out)
{
    const Json* v = o.get(key);
    if (v && v->type == Json::Number && std::isfinite(v->num)) out = v->num;
}
void take(const Json& o, const char* key, bool& out)
{
    const Json* v = o.get(key);
    if (v && v->type == Json::Bool) out = v->b;
}

// --- one HTTP request through curl's multi interface ----------------------------
//
// The multi loop wakes at least every 50 ms, so a stop or cancel flag is seen
// quickly even when the connection is idle or still connecting.

struct Request {
    std::string url;
    bool post = false;
    long total_timeout_ms = 0;   // 0: none
    int idle_timeout_ms = 0;     // 0: none
    const std::atomic_bool* stop = nullptr;
    const std::atomic_bool* cancel = nullptr;
    // Body bytes of a 2xx answer; return false to end the transfer early.
    std::function<bool(const char*, size_t)> on_body;
};

struct Result {
    enum Kind { Done, Stopped, CallerEnded, IdleTimeout, HttpError, TransportError } kind = Done;
    long status = 0;
    std::string error;   // filled for HttpError / TransportError / IdleTimeout
};

struct WriteState {
    CURL* easy = nullptr;
    const Request* req = nullptr;
    bool checked = false;
    bool ok_status = false;
    bool caller_ended = false;
    std::string error_body;
    Clock::time_point last_data;
};

size_t on_write(char* data, size_t size, size_t count, void* user)
{
    WriteState* st = (WriteState*)user;
    size_t n = size * count;
    st->last_data = Clock::now();
    if (!st->checked) {
        long code = 0;
        curl_easy_getinfo(st->easy, CURLINFO_RESPONSE_CODE, &code);
        st->ok_status = code >= 200 && code < 300;
        st->checked = true;
    }
    if (!st->ok_status) {
        if (st->error_body.size() < 512) st->error_body.append(data, std::min<size_t>(n, 512));
        return n;
    }
    if (st->req->on_body && !st->req->on_body(data, n)) {
        st->caller_ended = true;
        return 0;   // aborts the transfer
    }
    return n;
}

std::string describe(CURLcode code, const char* detail)
{
    switch (code) {
    case CURLE_COULDNT_CONNECT: return "the connection was refused or the host can't be reached";
    case CURLE_OPERATION_TIMEDOUT: return "the request timed out";
    case CURLE_COULDNT_RESOLVE_HOST: return "the host name could not be found";
    case CURLE_URL_MALFORMAT: return "the address is not valid";
    case CURLE_GOT_NOTHING: return "the server closed the connection without an answer";
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR: return "the connection was lost";
    default: break;
    }
    if (detail && *detail) return detail;
    return curl_easy_strerror(code);
}

std::string trim_body(std::string body)
{
    while (!body.empty() && std::strchr(" \t\r\n", body.back())) body.pop_back();
    return body;
}

Result perform(const Request& req)
{
    curl_setup();
    Result result;
    CURL* easy = curl_easy_init();
    CURLM* multi = curl_multi_init();
    if (!easy || !multi) {
        if (easy) curl_easy_cleanup(easy);
        if (multi) curl_multi_cleanup(multi);
        result.kind = Result::TransportError;
        result.error = "the network library could not start";
        return result;
    }
    char errbuf[CURL_ERROR_SIZE] = {};
    WriteState st;
    st.easy = easy;
    st.req = &req;
    st.last_data = Clock::now();

    curl_easy_setopt(easy, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(easy, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(easy, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(easy, CURLOPT_BUFFERSIZE, 256L * 1024);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &st);
    if (req.total_timeout_ms > 0) curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, req.total_timeout_ms);
    if (req.post) {
        curl_easy_setopt(easy, CURLOPT_POST, 1L);
        curl_easy_setopt(easy, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, 0L);
    }
    curl_multi_add_handle(multi, easy);

    bool finished = false;
    CURLcode code = CURLE_OK;
    for (;;) {
        int running = 0;
        curl_multi_perform(multi, &running);
        int left = 0;
        while (CURLMsg* msg = curl_multi_info_read(multi, &left)) {
            if (msg->msg == CURLMSG_DONE) {
                finished = true;
                code = msg->data.result;
            }
        }
        if (finished || running == 0) break;
        if ((req.stop && req.stop->load()) || (req.cancel && req.cancel->load())) {
            result.kind = Result::Stopped;
            break;
        }
        if (req.idle_timeout_ms > 0 &&
            Clock::now() - st.last_data > std::chrono::milliseconds(req.idle_timeout_ms)) {
            result.kind = Result::IdleTimeout;
            result.error = "no data arrived for " + std::to_string((req.idle_timeout_ms + 500) / 1000) + " s";
            break;
        }
        curl_multi_poll(multi, nullptr, 0, 50, nullptr);
    }

    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &result.status);
    if (finished) {
        if (st.caller_ended) {
            result.kind = Result::CallerEnded;
        } else if (code != CURLE_OK) {
            result.kind = Result::TransportError;
            result.error = describe(code, errbuf);
        } else if (result.status < 200 || result.status >= 300) {
            result.kind = Result::HttpError;
            std::string body = trim_body(st.error_body);
            result.error = "HTTP " + std::to_string(result.status) + (body.empty() ? "" : ": " + body);
        }
    }
    curl_multi_remove_handle(multi, easy);
    curl_easy_cleanup(easy);
    curl_multi_cleanup(multi);
    return result;
}

std::string normalize_base(std::string base)
{
    while (!base.empty() && std::strchr(" \t\r\n", base.back())) base.pop_back();
    size_t start = base.find_first_not_of(" \t\r\n");
    base = start == std::string::npos ? std::string() : base.substr(start);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!base.empty() && base.find("://") == std::string::npos) base = "http://" + base;
    return base;
}

}  // namespace

PayloadClient::PayloadClient(std::string base_url) : base_(normalize_base(std::move(base_url))) {}

bool PayloadClient::fetch_json(const std::string& path, std::string& body, std::string& error)
{
    Request req;
    req.url = base_ + path;
    req.total_timeout_ms = 5000;
    req.cancel = cancel_;
    body.clear();
    req.on_body = [&](const char* d, size_t n) {
        if (body.size() > (1u << 20)) return false;
        body.append(d, n);
        return true;
    };
    Result r = perform(req);
    if (r.kind == Result::Stopped) {
        error = "cancelled";
        return false;
    }
    if (r.kind == Result::CallerEnded) {
        error = "the answer is too large";
        return false;
    }
    if (r.kind != Result::Done) {
        error = r.error;
        return false;
    }
    return true;
}

bool PayloadClient::fetch_config(StreamConfig& config, std::string& error)
{
    std::string body;
    if (!fetch_json("/config.json", body, error)) return false;
    Json j;
    if (!JsonParser(body).parse(j) || j.type != Json::Object) {
        error = "the console sent a config that could not be read";
        return false;
    }
    StreamConfig c;
    take(j, "platform", c.platform);
    take(j, "width", c.width);
    take(j, "height", c.height);
    take(j, "fps", c.fps);
    take(j, "codec", c.codec);
    take(j, "sps", c.sps);
    take(j, "pps", c.pps);
    const Json* audio = j.get("audio");
    if (audio && audio->type == Json::Object) {
        c.has_audio = true;
        take(*audio, "codec", c.audio_codec);
        take(*audio, "channels", c.audio_channels);
        take(*audio, "rate", c.audio_rate);
        take(*audio, "bits", c.audio_bits);
    }
    const Json* res = j.get("resolutions");
    if (res && res->type == Json::Array)
        for (const Json& item : res->items)
            if (item.type == Json::Number) c.resolutions.push_back((int)item.num);
    take(j, "requested_height", c.requested_height);
    take(j, "profile_changing", c.profile_changing);
    take(j, "profile_error", c.profile_error);
    config = c;
    return true;
}

bool PayloadClient::fetch_status(StreamStatus& status, std::string& error)
{
    std::string body;
    if (!fetch_json("/status.json", body, error)) return false;
    Json j;
    if (!JsonParser(body).parse(j) || j.type != Json::Object) {
        error = "the console sent a status that could not be read";
        return false;
    }
    StreamStatus s;
    take(j, "backend", s.backend);
    take(j, "width", s.width);
    take(j, "height", s.height);
    take(j, "frames", s.frames);
    take(j, "bytes", s.bytes);
    take(j, "fps", s.fps);
    take(j, "kbps", s.kbps);
    take(j, "clients", s.clients);
    take(j, "viewers", s.viewers);
    take(j, "recording", s.recording);
    take(j, "recording_name", s.recording_name);
    take(j, "recording_frames", s.recording_frames);
    take(j, "recording_bytes", s.recording_bytes);
    take(j, "recording_seconds", s.recording_seconds);
    take(j, "recording_error", s.recording_error);
    take(j, "audio_chunks", s.audio_chunks);
    take(j, "audio_bytes", s.audio_bytes);
    take(j, "audio_kbps", s.audio_kbps);
    take(j, "audio_codec", s.audio_codec);
    status = s;
    return true;
}

bool PayloadClient::post(const std::string& path, std::string* body, std::string& error)
{
    Request req;
    req.url = base_ + path;
    req.post = true;
    req.total_timeout_ms = 8000;
    req.cancel = cancel_;
    std::string text;
    req.on_body = [&](const char* d, size_t n) {
        if (text.size() < (1u << 20)) text.append(d, n);
        return true;
    };
    Result r = perform(req);
    if (r.kind == Result::Stopped) {
        error = "cancelled";
        return false;
    }
    if (r.kind != Result::Done) {
        error = r.error;
        return false;
    }
    if (body) *body = text;
    return true;
}

bool PayloadClient::stream_records(const std::string& path, size_t max_len,
                                   const std::function<bool(const uint8_t*, size_t, uint32_t)>& callback,
                                   std::atomic_bool& stop, std::string& error, int idle_timeout_ms)
{
    error.clear();
    std::vector<uint8_t> buf;
    size_t head = 0;   // start of unparsed bytes in buf
    bool bad_framing = false;

    Request req;
    req.url = base_ + path;
    req.idle_timeout_ms = idle_timeout_ms > 0 ? idle_timeout_ms : 0;
    req.stop = &stop;
    req.cancel = cancel_;
    req.on_body = [&](const char* d, size_t n) {
        buf.insert(buf.end(), (const uint8_t*)d, (const uint8_t*)d + n);
        for (;;) {
            size_t avail = buf.size() - head;
            if (avail < 8) break;
            const uint8_t* p = buf.data() + head;
            size_t len = ((size_t)p[0] << 24) | ((size_t)p[1] << 16) | ((size_t)p[2] << 8) | p[3];
            uint32_t pts = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | p[7];
            if (len == 0 || len > max_len) {
                bad_framing = true;
                return false;
            }
            if (avail < 8 + len) break;
            if (!callback(p + 8, len, pts)) return false;
            if (stop.load()) return false;
            head += 8 + len;
        }
        if (head > 0 && (head == buf.size() || head > (1u << 20))) {
            buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)head);
            head = 0;
        }
        return true;
    };

    Result r = perform(req);
    if (bad_framing) {
        error = "the stream has invalid framing";
        return false;
    }
    switch (r.kind) {
    case Result::Done:
    case Result::Stopped:
    case Result::CallerEnded:
        return true;
    case Result::HttpError:
        error = r.status == 409 ? std::string(kTakenOver) : r.error;
        return false;
    default:
        error = r.error;
        return false;
    }
}

bool PayloadClient::stream_frames(const FrameCallback& callback, std::atomic_bool& stop, std::string& error,
                                  int idle_timeout_ms)
{
    return stream_records("/frames?take=1", 2097152, callback, stop, error, idle_timeout_ms);
}

bool PayloadClient::stream_audio(const AudioCallback& callback, std::atomic_bool& stop, std::string& error,
                                 int idle_timeout_ms)
{
    return stream_records("/audio", 65536, callback, stop, error, idle_timeout_ms);
}

}
