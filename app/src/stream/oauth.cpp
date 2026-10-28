#include "oauth.h"

#include "destinations.h"
#include "media.h"
#include "glint/payload_client.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace glint {
namespace {

using Clock = std::chrono::steady_clock;

struct HttpResponse {
    long status = 0;
    std::string body;
    std::string error;
};

size_t collect(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

// Test override: every endpoint host replaced by a loopback mock.
std::string test_base() {
    const char* v = std::getenv("GLINT_OAUTH_TEST_BASE");
    if (!v) return "";
    std::string s = v;
    if (s.rfind("http://127.0.0.1:", 0) != 0 && s.rfind("http://localhost:", 0) != 0) return "";
    return s;
}

std::string endpoint(const std::string& real_host, const std::string& path) {
    std::string base = test_base();
    return (base.empty() ? real_host : base) + path;
}

HttpResponse http(const std::string& url, const std::string* form, const std::vector<std::string>& headers) {
    static bool inited = [] { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }();
    (void)inited;
    HttpResponse r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl_easy_init failed";
        return r;
    }
    curl_slist* list = nullptr;
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Glint/1.0");
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    if (list) curl_easy_setopt(c, CURLOPT_HTTPHEADER, list);
    if (form) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, form->c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)form->size());
    }
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_slist_free_all(list);
    curl_easy_cleanup(c);
    return r;
}

std::string esc(const std::string& v) {
    CURL* c = curl_easy_init();
    char* e = curl_easy_escape(c, v.c_str(), (int)v.size());
    std::string out = e ? e : "";
    curl_free(e);
    curl_easy_cleanup(c);
    return out;
}

std::string unesc(const std::string& v) {
    CURL* c = curl_easy_init();
    int n = 0;
    char* d = curl_easy_unescape(c, v.c_str(), (int)v.size(), &n);
    std::string out = d ? std::string(d, (size_t)n) : "";
    curl_free(d);
    curl_easy_cleanup(c);
    return out;
}

bool random_bytes(unsigned char* out, size_t n) {
    return BCryptGenRandom(nullptr, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::string random_token(size_t length) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::vector<unsigned char> raw(length);
    if (!random_bytes(raw.data(), raw.size())) return "";
    std::string s;
    for (unsigned char b : raw) s.push_back(alphabet[b % 66]);
    return s;
}

std::string base64url(const unsigned char* data, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16 | (i + 1 < n ? (uint32_t)data[i + 1] << 8 : 0) |
                     (i + 2 < n ? (uint32_t)data[i + 2] : 0);
        out.push_back(t[(v >> 18) & 63]);
        out.push_back(t[(v >> 12) & 63]);
        if (i + 1 < n) out.push_back(t[(v >> 6) & 63]);
        if (i + 2 < n) out.push_back(t[v & 63]);
    }
    return out;
}

bool sha256(const std::string& text, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(hash, (PUCHAR)text.data(), (ULONG)text.size(), 0) == 0 &&
              BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string query_param(const std::string& query, const std::string& name, int* count = nullptr) {
    std::string value;
    int seen = 0;
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string part = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == name) {
            value = unesc(part.substr(eq + 1));
            seen++;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    if (count) *count = seen;
    return value;
}

} // namespace

AccountLinker::~AccountLinker() {
    cancel();
}

bool AccountLinker::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return link_.state == AccountLink::State::Starting || link_.state == AccountLink::State::WaitingForUser ||
           link_.state == AccountLink::State::Fetching;
}

std::string AccountLinker::platform() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return platform_;
}

AccountLink AccountLinker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    AccountLink copy = link_;
    copy.stream_key.clear();
    return copy;
}

bool AccountLinker::take_result(AccountLink& result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (link_.state != AccountLink::State::Done || result_taken_) return false;
    result = link_;
    result_taken_ = true;
    link_.stream_key.clear();
    return true;
}

void AccountLinker::cancel() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    if (link_.state != AccountLink::State::Done && link_.state != AccountLink::State::Failed)
        link_ = AccountLink{};
}

void AccountLinker::update(AccountLink::State state, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    link_.state = state;
    link_.message = message;
}

void AccountLinker::failed(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancel_) {
        link_ = AccountLink{};
        return;
    }
    link_.state = AccountLink::State::Failed;
    link_.error = error;
    link_.message = error;
}

bool AccountLinker::start_twitch(const std::string& client_id, std::string& error) {
    if (busy()) {
        error = "Account authorization is already in progress or the Client ID is invalid.";
        return false;
    }
    if (client_id.empty()) {
        error = "Register a streaming OAuth application first and enter its Client ID.";
        return false;
    }
    if (client_id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) {
        error = "OAuth Client ID contains invalid characters.";
        return false;
    }
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        link_ = AccountLink{};
        link_.state = AccountLink::State::Starting;
        link_.message = "Contacting Twitch...";
        platform_ = "twitch";
        result_taken_ = false;
    }
    thread_ = std::thread(&AccountLinker::run_twitch, this, client_id);
    return true;
}

void AccountLinker::run_twitch(std::string client_id) {
    const std::string id_host = "https://id.twitch.tv", api_host = "https://api.twitch.tv";
    const std::string scope = "channel:read:stream_key";
    std::string form = "client_id=" + esc(client_id) + "&scopes=" + esc(scope);
    HttpResponse r = http(endpoint(id_host, "/oauth2/device"), &form, {});
    if (!r.error.empty()) return failed("Could not reach Twitch: " + r.error);
    std::string device_code = json_string(r.body, "device_code");
    std::string user_code = json_string(r.body, "user_code");
    std::string uri = json_string(r.body, "verification_uri");
    long long interval = json_int(r.body, "interval", 5);
    long long expires = json_int(r.body, "expires_in", 1800);
    if (r.status != 200 || device_code.empty() || user_code.empty()) {
        std::string m = json_string(r.body, "message");
        return failed(m.empty() ? "Invalid OAuth client ID" : "Twitch refused the request: " + m);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        link_.state = AccountLink::State::WaitingForUser;
        link_.user_code = user_code;
        link_.verification_uri = uri.empty() ? "https://www.twitch.tv/activate" : uri;
        link_.message = "Open Twitch activation page and enter the displayed code.";
    }

    // Poll the token endpoint until the user approves (or gives up).
    std::string token;
    auto deadline = Clock::now() + std::chrono::seconds(expires);
    std::string poll = "client_id=" + esc(client_id) + "&scopes=" + esc(scope) + "&device_code=" +
                       esc(device_code) + "&grant_type=" + esc("urn:ietf:params:oauth:grant-type:device_code");
    while (!cancel_ && Clock::now() < deadline) {
        for (long long i = 0; i < interval * 10 && !cancel_; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (cancel_) break;
        HttpResponse t = http(endpoint(id_host, "/oauth2/token"), &poll, {});
        if (t.status == 200) {
            token = json_string(t.body, "access_token");
            if (!token.empty()) {
                std::string ignored;
                vault::write(vault::target("OAuth", "twitch"), t.body, ignored);   // access + refresh token
                break;
            }
        }
        std::string m = json_string(t.body, "message");
        if (m == "authorization_pending") continue;
        if (m == "slow_down") {
            interval += 5;
            continue;
        }
        if (!t.error.empty()) continue;   // transient network error: keep polling
        return failed(m == "access_denied" ? "Twitch authorization was declined."
                      : m.empty()          ? "Invalid OAuth token response"
                                           : "Twitch authorization failed: " + m);
    }
    if (cancel_) return failed("cancelled");
    if (token.empty()) return failed("The Twitch code expired. Start again.");

    update(AccountLink::State::Fetching, "Account connected. Retrieving your broadcast configuration...");
    std::vector<std::string> headers = {"Authorization: Bearer " + token, "Client-Id: " + client_id};
    HttpResponse u = http(endpoint(api_host, "/helix/users"), nullptr, headers);
    std::string user_id = json_string(u.body, "id");
    std::string login = json_string(u.body, "login");
    if (u.status != 200 || user_id.empty()) return failed("Invalid Twitch broadcaster identity");
    HttpResponse k = http(endpoint(api_host, "/helix/streams/key?broadcaster_id=" + esc(user_id)), nullptr, headers);
    std::string key = json_string(k.body, "stream_key");
    if (k.status != 200 || key.empty())
        return failed("Could not retrieve Twitch stream key. Check channel permissions and authorization.");

    std::lock_guard<std::mutex> lock(mutex_);
    link_.state = AccountLink::State::Done;
    link_.message = "Account connected. Your broadcasting destination is ready.";
    link_.account = login;
    link_.server = preset_server("twitch");
    link_.stream_key = key;
}

bool AccountLinker::start_youtube(const std::string& client_id, const std::string& client_secret,
                                  std::string& error) {
    if (busy()) {
        error = "Account authorization is already in progress or the Client ID is invalid.";
        return false;
    }
    if (client_id.empty()) {
        error = "Register a streaming OAuth application first and enter its Client ID.";
        return false;
    }
    if (client_id.find_first_of(" \t\r\n&?#") != std::string::npos) {
        error = "OAuth Client ID contains invalid characters.";
        return false;
    }
    if (thread_.joinable()) thread_.join();
    cancel_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        link_ = AccountLink{};
        link_.state = AccountLink::State::Starting;
        link_.message = "Opening Google sign-in...";
        platform_ = "youtube";
        result_taken_ = false;
    }
    thread_ = std::thread(&AccountLinker::run_youtube, this, client_id, client_secret);
    return true;
}

void AccountLinker::run_youtube(std::string client_id, std::string client_secret) {
    const std::string scope = "https://www.googleapis.com/auth/youtube.readonly";
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alen = sizeof(a);
    if (l == INVALID_SOCKET || bind(l, (sockaddr*)&a, sizeof(a)) != 0 || listen(l, 4) != 0 ||
        getsockname(l, (sockaddr*)&a, &alen) != 0) {
        if (l != INVALID_SOCKET) closesocket(l);
        return failed("No active YouTube loopback callback listener");
    }
    std::string redirect = "http://127.0.0.1:" + std::to_string(ntohs(a.sin_port));
    std::string verifier = random_token(64), state = random_token(32);
    unsigned char digest[32];
    if (verifier.empty() || state.empty() || !sha256(verifier, digest)) {
        closesocket(l);
        return failed("Could not generate secure OAuth challenge");
    }
    std::string challenge = base64url(digest, 32);
    std::string auth = endpoint("https://accounts.google.com", "/o/oauth2/v2/auth") + "?client_id=" + esc(client_id) +
                       "&redirect_uri=" + esc(redirect) + "&response_type=code&scope=" + esc(scope) +
                       "&code_challenge=" + challenge + "&code_challenge_method=S256&state=" + state +
                       "&access_type=offline&prompt=consent";
    if (!test_base().empty()) {
        // Test: the mock plays the browser and redirects back to us.
        std::thread([auth] {
            CURL* c = curl_easy_init();
            curl_easy_setopt(c, CURLOPT_URL, auth.c_str());
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 10000L);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, +[](char*, size_t s, size_t n, void*) { return s * n; });
            curl_easy_perform(c);
            curl_easy_cleanup(c);
        }).detach();
    } else if (!open_with_default_app(auth)) {
        closesocket(l);
        return failed("Could not open the browser for Google sign-in.");
    }
    update(AccountLink::State::WaitingForUser, "Approve YouTube access in your browser and return to Glint.");

    // Wait for the redirect (5 minutes), ignoring stray requests like /favicon.ico.
    std::string code;
    auto deadline = Clock::now() + std::chrono::minutes(5);
    while (!cancel_ && Clock::now() < deadline && code.empty()) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(l, &r);
        timeval tv = {0, 200000};
        if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(l, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        DWORD timeout_ms = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
            int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, (size_t)n);
        }
        std::string line = req.substr(0, req.find("\r\n"));
        std::string error_text;
        bool ours = false;
        if (line.rfind("GET /", 0) == 0) {
            size_t q = line.find('?');
            size_t sp = line.find(' ', 4);
            std::string query = q != std::string::npos && q < sp ? line.substr(q + 1, sp - q - 1) : "";
            int n_state = 0, n_code = 0;
            std::string got_state = query_param(query, "state", &n_state);
            std::string got_code = query_param(query, "code", &n_code);
            std::string err = query_param(query, "error");
            if (!query.empty() && (n_state || n_code || !err.empty())) {
                ours = true;
                if (n_state != 1 || n_code > 1) error_text = "Duplicate or invalid OAuth callback parameter";
                else if (got_state != state) error_text = "Invalid OAuth callback";
                else if (!err.empty()) error_text = err == "access_denied" ? "Google sign-in was declined." : "Google sign-in failed: " + err;
                else if (got_code.empty()) error_text = "Invalid OAuth callback";
                else code = got_code;
            }
        }
        std::string body = ours ? (error_text.empty()
                                       ? "<h3>Glint is connected to YouTube.</h3><p>You can close this tab.</p>"
                                       : "<h3>Glint could not connect to YouTube.</h3><p>Return to Glint.</p>")
                                : "";
        std::string resp = std::string(ours ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") +
                           "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
        send(c, resp.data(), (int)resp.size(), 0);
        closesocket(c);
        if (ours && !error_text.empty()) {
            closesocket(l);
            return failed(error_text);
        }
    }
    closesocket(l);
    if (cancel_) return failed("cancelled");
    if (code.empty()) return failed("No pending YouTube authorization");

    update(AccountLink::State::Fetching, "Account connected. Retrieving your broadcast configuration...");
    std::string form = "code=" + esc(code) + "&client_id=" + esc(client_id) + "&code_verifier=" + esc(verifier) +
                       "&grant_type=authorization_code&redirect_uri=" + esc(redirect);
    if (!client_secret.empty()) form += "&client_secret=" + esc(client_secret);
    HttpResponse t = http(endpoint("https://oauth2.googleapis.com", "/token"), &form, {});
    std::string token = json_string(t.body, "access_token");
    if (t.status != 200 || token.empty())
        return failed("Google rejected the authorization code. Verify desktop OAuth client and redirect settings.");
    std::string granted = json_string(t.body, "scope");
    if (granted.find("youtube.readonly") == std::string::npos && granted.find("auth/youtube") == std::string::npos)
        return failed("Google did not grant the YouTube read-only permission.");
    std::string ignored;
    vault::write(vault::target("OAuth", "youtube"), t.body, ignored);

    HttpResponse s = http(endpoint("https://www.googleapis.com", "/youtube/v3/liveStreams?part=cdn&mine=true&maxResults=50"),
                          nullptr, {"Authorization: Bearer " + token});
    if (s.status != 200) return failed("Could not retrieve YouTube live stream ingestion information.");
    std::string server = json_string(s.body, "rtmpsIngestionAddress");
    std::string key = json_string(s.body, "streamName");
    if (server.empty() || key.empty())
        return failed("No accessible RTMPS YouTube streams. Enable live streaming and create a stream in YouTube Studio.");

    std::lock_guard<std::mutex> lock(mutex_);
    link_.state = AccountLink::State::Done;
    link_.message = "Account connected. Your broadcasting destination is ready.";
    link_.account = "YouTube";
    link_.server = server;
    link_.stream_key = key;
}

} // namespace glint
