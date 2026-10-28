#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace glint {

// Sign-in flows that turn an account into a ready destination (server + key).
// Both use the user's own registered OAuth application (client ID). Tokens go
// to the vault (Glint/OAuth/v1:<platform>).
//
// For tests, $GLINT_OAUTH_TEST_BASE (loopback http:// only) replaces every
// Twitch/Google endpoint host; the YouTube browser step is then simulated.
struct AccountLink {
    enum class State { Idle, Starting, WaitingForUser, Fetching, Done, Failed };
    State state = State::Idle;
    std::string message;           // user-facing status line
    std::string error;
    std::string user_code;         // Twitch: code to enter
    std::string verification_uri;  // Twitch: activation page
    std::string account;           // login / channel shown after success
    std::string server;            // ingest server (rtmps://...)
    std::string stream_key;        // secret: never logged or shown
};

class AccountLinker {
public:
    AccountLinker() = default;
    ~AccountLinker();

    AccountLinker(const AccountLinker&) = delete;
    AccountLinker& operator=(const AccountLinker&) = delete;

    bool start_twitch(const std::string& client_id, std::string& error);
    bool start_youtube(const std::string& client_id, const std::string& client_secret, std::string& error);
    void cancel();
    bool busy() const;
    std::string platform() const;
    AccountLink snapshot() const;          // stream_key is blanked
    // Once Done, hands out the result (with the key) exactly once.
    bool take_result(AccountLink& result);

private:
    void run_twitch(std::string client_id);
    void run_youtube(std::string client_id, std::string client_secret);
    void update(AccountLink::State state, const std::string& message);
    void failed(const std::string& error);

    mutable std::mutex mutex_;
    AccountLink link_;
    std::string platform_;
    bool result_taken_ = true;
    std::thread thread_;
    std::atomic_bool cancel_{false};
};

} // namespace glint
