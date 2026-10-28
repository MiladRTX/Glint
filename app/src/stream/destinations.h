#pragma once

#include "settings.h"

#include <string>
#include <vector>

namespace glint {

// Windows Credential Manager (generic credentials). Secrets never go to files
// or logs. Target names: "Glint/Streaming/v1:<id>", "Glint/OAuth/v1:<name>"
// ($GLINT_VAULT_PREFIX replaces "Glint" in tests).
namespace vault {
std::string target(const std::string& area, const std::string& name);   // area: "Streaming" / "OAuth"
bool write(const std::string& target, const std::string& secret, std::string& error);
bool read(const std::string& target, std::string& secret);
bool remove(const std::string& target);
} // namespace vault

struct StreamDestination {
    std::string id;         // stable, used for the vault target
    std::string platform;   // "twitch", "youtube", "tiktok", "custom"
    std::string name;
    std::string server;     // rtmp:// or rtmps:// ingest without the key
    bool enabled = true;
};

const char* platform_label(const std::string& platform);
std::string preset_server(const std::string& platform);   // empty for tiktok/custom
// server + "/" + key, the URL the broadcaster connects to (contains the secret).
std::string ingest_url(const std::string& server, const std::string& key);

struct StreamQuality {
    int height = 1080;
    int fps = 60;
    int video_kbps = 6000;
};

class DestinationStore {
public:
    bool load(std::string& error);
    bool save(std::string& error) const;
    const std::string& path() const { return path_; }

    std::vector<StreamDestination>& items() { return items_; }
    const std::vector<StreamDestination>& items() const { return items_; }
    StreamDestination* find(const std::string& id);
    StreamDestination& add(const std::string& platform);
    void remove(const std::string& id);   // also deletes its key

    bool set_key(const std::string& id, const std::string& key, std::string& error);
    bool has_key(const std::string& id) const;
    std::string key(const std::string& id) const;

    StreamQuality quality;

private:
    std::string path_;
    std::vector<StreamDestination> items_;
};

// Registered OAuth applications (client IDs only; secrets in the vault).
struct OAuthApps {
    std::string twitch_client_id;
    std::string youtube_client_id;
};
bool load_oauth_apps(OAuthApps& apps, std::string& error);
bool save_oauth_apps(const OAuthApps& apps, std::string& error);

} // namespace glint
