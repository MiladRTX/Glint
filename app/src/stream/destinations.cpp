#include "destinations.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincred.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace glint {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string trim(const std::string& v) {
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

bool write_atomic(const fs::path& path, const std::string& text, std::string& error) {
    return write_file_atomic(path.u8string(), text, error);
}

bool valid_field(const std::string& v) {
    return v.find('\n') == std::string::npos && v.find('\r') == std::string::npos;
}

} // namespace

// --- vault -----------------------------------------------------------------

namespace vault {

static const char* kRoot = "Glint";

std::string target(const std::string& area, const std::string& name) {
    const char* prefix = std::getenv("GLINT_VAULT_PREFIX");
    std::string root = (prefix && *prefix) ? prefix : kRoot;
    return root + "/" + area + "/v1:" + name;
}


bool write(const std::string& target, const std::string& secret, std::string& error) {
    std::wstring t = widen(target);
    std::wstring user = L"Glint";
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = &t[0];
    cred.CredentialBlobSize = (DWORD)secret.size();
    cred.CredentialBlob = (LPBYTE)secret.data();
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = &user[0];
    if (!CredWriteW(&cred, 0)) {
        error = "Cannot save the secret in Windows Credential Manager (error " +
                std::to_string(GetLastError()) + ").";
        return false;
    }
    return true;
}

bool read(const std::string& target, std::string& secret) {
    PCREDENTIALW cred = nullptr;
    std::wstring t = widen(target);
    if (!CredReadW(t.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        // Saved before the rename, under Screenlink/...
        std::string root = std::string(kRoot) + "/";
        if (target.rfind(root, 0) != 0) return false;
        std::wstring old = widen("Screenlink/" + target.substr(root.size()));
        if (!CredReadW(old.c_str(), CRED_TYPE_GENERIC, 0, &cred)) return false;
    }
    secret.assign((const char*)cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    // Credentials made by other tools (cmdkey, Control Panel) hold UTF-16 text.
    bool utf16 = secret.size() >= 2 && secret.size() % 2 == 0;
    for (size_t i = 1; utf16 && i < secret.size(); i += 2) utf16 = secret[i] == '\0';
    if (utf16) {
        std::wstring w((const wchar_t*)secret.data(), secret.size() / 2);
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string out((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
        secret = out;
    }
    return true;
}

bool remove(const std::string& target) {
    std::wstring t = widen(target);
    return CredDeleteW(t.c_str(), CRED_TYPE_GENERIC, 0) != FALSE;
}

} // namespace vault

// --- presets ---------------------------------------------------------------

const char* platform_label(const std::string& platform) {
    if (platform == "twitch") return "Twitch";
    if (platform == "youtube") return "YouTube";
    if (platform == "tiktok") return "TikTok";
    return "Custom RTMP";
}

std::string preset_server(const std::string& platform) {
    if (platform == "twitch") return "rtmps://live.twitch.tv/app";
    if (platform == "youtube") return "rtmps://a.rtmps.youtube.com/live2";
    return "";
}

std::string ingest_url(const std::string& server, const std::string& key) {
    std::string s = trim(server);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s + "/" + trim(key);
}

// --- store -----------------------------------------------------------------

static const char* kHeader = "GLINT-DESTINATIONS-1";

bool DestinationStore::load(std::string& error) {
    path_ = (fs::u8path(settings_dir()) / "destinations.conf").u8string();
    items_.clear();
    quality = StreamQuality{};
    std::ifstream in(fs::u8path(path_), std::ios::binary);
    if (!in) return true;   // first run
    std::string line;
    if (!std::getline(in, line) || !is_header(trim(line), kHeader)) {
        error = "Invalid stream destination configuration";
        return false;
    }
    StreamDestination* current = nullptr;
    bool in_quality = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "[destination]") {
            items_.push_back(StreamDestination{});
            current = &items_.back();
            in_quality = false;
            continue;
        }
        if (line == "[quality]") {
            current = nullptr;
            in_quality = true;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (in_quality) {
            int n = std::atoi(v.c_str());
            if (k == "height" && (n == 480 || n == 720 || n == 1080)) quality.height = n;
            else if (k == "fps" && (n == 30 || n == 60)) quality.fps = n;
            else if (k == "video_kbps" && n >= 500 && n <= 20000) quality.video_kbps = n;
        } else if (current) {
            if (k == "id") current->id = v;
            else if (k == "platform") current->platform = v;
            else if (k == "name") current->name = v;
            else if (k == "server") current->server = v;
            else if (k == "enabled") current->enabled = v == "1";
        }
    }
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [](const StreamDestination& d) { return d.id.empty(); }),
                 items_.end());
    return true;
}

bool DestinationStore::save(std::string& error) const {
    std::ostringstream out;
    out << kHeader << "\n";
    out << "# Stream keys are stored in Windows Credential Manager, never in this file.\n";
    out << "[quality]\nheight=" << quality.height << "\nfps=" << quality.fps
        << "\nvideo_kbps=" << quality.video_kbps << "\n";
    for (const auto& d : items_) {
        if (!valid_field(d.name) || !valid_field(d.server) || !valid_field(d.platform)) {
            error = "Invalid stream destination configuration";
            return false;
        }
        out << "[destination]\nid=" << d.id << "\nplatform=" << d.platform << "\nname=" << d.name
            << "\nserver=" << d.server << "\nenabled=" << (d.enabled ? 1 : 0) << "\n";
    }
    std::string p = path_.empty() ? (fs::u8path(settings_dir()) / "destinations.conf").u8string() : path_;
    return write_atomic(fs::u8path(p), out.str(), error);
}

StreamDestination* DestinationStore::find(const std::string& id) {
    for (auto& d : items_)
        if (d.id == id) return &d;
    return nullptr;
}

StreamDestination& DestinationStore::add(const std::string& platform) {
    std::random_device rd;
    std::mt19937_64 rng(rd() ^ (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count());
    char id[24];
    std::snprintf(id, sizeof(id), "%016llx", (unsigned long long)rng());
    StreamDestination d;
    d.id = id;
    d.platform = platform;
    d.name = platform_label(platform);
    d.server = preset_server(platform);
    d.enabled = true;
    items_.push_back(d);
    return items_.back();
}

void DestinationStore::remove(const std::string& id) {
    vault::remove(vault::target("Streaming", id));
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const StreamDestination& d) { return d.id == id; }),
                 items_.end());
}

bool DestinationStore::set_key(const std::string& id, const std::string& key, std::string& error) {
    std::string k = trim(key);
    if (k.empty() || k.find_first_of(" \t\r\n") != std::string::npos) {
        error = "A stream key cannot be empty or contain spaces.";
        return false;
    }
    return vault::write(vault::target("Streaming", id), k, error);
}

bool DestinationStore::has_key(const std::string& id) const {
    std::string ignored;
    return vault::read(vault::target("Streaming", id), ignored);
}

std::string DestinationStore::key(const std::string& id) const {
    std::string secret;
    vault::read(vault::target("Streaming", id), secret);
    return secret;
}

// --- OAuth apps --------------------------------------------------------------

bool load_oauth_apps(OAuthApps& apps, std::string& error) {
    apps = OAuthApps{};
    std::ifstream in(fs::u8path(settings_dir()) / "oauth-apps.conf", std::ios::binary);
    if (!in) return true;
    std::string line;
    if (!std::getline(in, line) || !is_header(trim(line), "GLINT-OAUTH-APPS-1")) {
        error = "Saved OAuth credentials are invalid";
        return false;
    }
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "twitch_client_id") apps.twitch_client_id = v;
        else if (k == "youtube_client_id") apps.youtube_client_id = v;
    }
    return true;
}

bool save_oauth_apps(const OAuthApps& apps, std::string& error) {
    for (const std::string* v : {&apps.twitch_client_id, &apps.youtube_client_id})
        if (!valid_field(*v) || v->find_first_of(" \t") != std::string::npos) {
            error = "OAuth Client ID contains invalid characters.";
            return false;
        }
    std::ostringstream out;
    out << "GLINT-OAUTH-APPS-1\n";
    out << "# Client IDs of your own registered OAuth applications. Secrets and tokens live in\n";
    out << "# Windows Credential Manager.\n";
    out << "twitch_client_id=" << apps.twitch_client_id << "\n";
    out << "youtube_client_id=" << apps.youtube_client_id << "\n";
    if (!write_atomic(fs::u8path(settings_dir()) / "oauth-apps.conf", out.str(), error)) {
        error = "Could not save OAuth application settings.";
        return false;
    }
    return true;
}

} // namespace glint
