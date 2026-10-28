#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace glint {

// RTMPS through Windows TLS.
//
// librtmp's own TLS (in this FFmpeg build) neither verifies the server
// certificate nor completes TLS 1.3 handshakes. For rtmps:// destinations the
// broadcaster instead points librtmp at a loopback port served by TlsRelay,
// which opens the real connection with Schannel: certificate chain and host
// name are checked against the Windows trust store, and bytes are relayed
// both ways until either side closes.
class TlsRelay {
public:
    TlsRelay() = default;
    ~TlsRelay();

    TlsRelay(const TlsRelay&) = delete;
    TlsRelay& operator=(const TlsRelay&) = delete;

    // Listens on 127.0.0.1:<ephemeral>; the TLS connection to host:port is
    // made when the first (and only) local client connects.
    bool start(const std::string& host, int port, std::string& error);
    int local_port() const { return local_port_; }
    void stop();
    std::string error() const;

private:
    void run();

    std::string host_;
    int port_ = 443;
    int local_port_ = 0;
    uintptr_t listener_ = ~(uintptr_t)0;
    std::thread thread_;
    std::atomic_bool stop_{false};
    mutable std::mutex mutex_;
    std::string error_;
};

// TLS handshake only (no data): checks that host:port presents a certificate
// Windows trusts. Used by tests and to give a clear error before going live.
bool tls_probe(const std::string& host, int port, std::string& error);

} // namespace glint
