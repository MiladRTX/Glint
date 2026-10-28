#include "tls_relay.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace glint {
namespace {

void ensure_winsock() {
    static bool done = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)done;
}

bool send_all(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
        int k = send(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

bool is_loopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// Only for local tests against a self-signed loopback server; never applies
// to a real destination.
bool allow_untrusted(const std::string& host) {
    const char* v = std::getenv("GLINT_TLS_TEST_ALLOW_UNTRUSTED");
    return is_loopback(host) && v && std::strcmp(v, "1") == 0;
}

std::string sec_text(SECURITY_STATUS s) {
    switch (s) {
    case SEC_E_UNTRUSTED_ROOT: return "the server's certificate is not trusted by Windows";
    case SEC_E_WRONG_PRINCIPAL: return "the server's certificate does not match its name";
    case SEC_E_CERT_EXPIRED: return "the server's certificate has expired";
    case CRYPT_E_REVOKED: return "the server's certificate has been revoked";
    case SEC_E_ILLEGAL_MESSAGE: return "the server sent an invalid TLS message";
    case SEC_E_ALGORITHM_MISMATCH: return "no common TLS settings with the server";
    case SEC_E_INTERNAL_ERROR: return "Windows TLS internal error";
    default: {
        char b[48];
        std::snprintf(b, sizeof(b), "TLS error 0x%08lx", (unsigned long)s);
        return b;
    }
    }
}

SOCKET tcp_connect(const std::string& host, int port, std::string& error) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || !result) {
        error = "cannot resolve " + host;
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = result; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, a->ai_addr, (int)a->ai_addrlen);
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(s, &w);
        FD_SET(s, &e);
        timeval tv = {10, 0};
        int r = select(0, nullptr, &w, &e, &tv);
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        if (r > 0 && FD_ISSET(s, &w) && !FD_ISSET(s, &e)) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(result);
    if (s == INVALID_SOCKET) error = "cannot reach " + host + ":" + service;
    else {
        BOOL one = TRUE;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    }
    return s;
}

struct Tls {
    CredHandle cred = {};
    CtxtHandle ctx = {};
    bool have_cred = false;
    bool have_ctx = false;
    SecPkgContext_StreamSizes sizes = {};
    std::vector<char> in;   // bytes from the server not yet decrypted

    ~Tls() {
        if (have_ctx) DeleteSecurityContext(&ctx);
        if (have_cred) FreeCredentialsHandle(&cred);
    }
};

bool recv_more(SOCKET s, std::vector<char>& buf) {
    char tmp[16384];
    int n = recv(s, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    buf.insert(buf.end(), tmp, tmp + n);
    return true;
}

bool tls_handshake(SOCKET s, const std::string& host, Tls& t, std::string& error) {
    bool manual = allow_untrusted(host);
    SCHANNEL_CRED sc = {};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO |
                 (manual ? SCH_CRED_MANUAL_CRED_VALIDATION
                         : (SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
                            SCH_CRED_IGNORE_NO_REVOCATION_CHECK | SCH_CRED_IGNORE_REVOCATION_OFFLINE));
    SECURITY_STATUS st = AcquireCredentialsHandleA(nullptr, (LPSTR)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, nullptr, &sc,
                                                   nullptr, nullptr, &t.cred, nullptr);
    if (st != SEC_E_OK) {
        error = "Windows TLS is unavailable (" + sec_text(st) + ")";
        return false;
    }
    t.have_cred = true;

    const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                        ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
    DWORD attrs = 0;
    SecBuffer out0 = {0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc outd0 = {SECBUFFER_VERSION, 1, &out0};
    st = InitializeSecurityContextA(&t.cred, nullptr, (SEC_CHAR*)host.c_str(), flags, 0, 0, nullptr, 0, &t.ctx,
                                    &outd0, &attrs, nullptr);
    if (st != SEC_I_CONTINUE_NEEDED) {
        error = sec_text(st);
        return false;
    }
    t.have_ctx = true;
    if (out0.cbBuffer && out0.pvBuffer) {
        bool ok = send_all(s, (const char*)out0.pvBuffer, out0.cbBuffer);
        FreeContextBuffer(out0.pvBuffer);
        if (!ok) {
            error = "connection closed during TLS handshake";
            return false;
        }
    }

    bool need_read = true;
    for (;;) {
        if (need_read && !recv_more(s, t.in)) {
            error = "connection closed during TLS handshake";
            return false;
        }
        SecBuffer inb[2] = {{(ULONG)t.in.size(), SECBUFFER_TOKEN, t.in.data()}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc ind = {SECBUFFER_VERSION, 2, inb};
        SecBuffer outb = {0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc outd = {SECBUFFER_VERSION, 1, &outb};
        st = InitializeSecurityContextA(&t.cred, &t.ctx, (SEC_CHAR*)host.c_str(), flags, 0, 0, &ind, 0, nullptr,
                                        &outd, &attrs, nullptr);
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            need_read = true;
            continue;
        }
        if (outb.cbBuffer && outb.pvBuffer) {
            bool ok = send_all(s, (const char*)outb.pvBuffer, outb.cbBuffer);
            FreeContextBuffer(outb.pvBuffer);
            if (!ok) {
                error = "connection closed during TLS handshake";
                return false;
            }
        }
        if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer > 0) {
            std::vector<char> extra(t.in.end() - inb[1].cbBuffer, t.in.end());
            t.in.swap(extra);
        } else {
            t.in.clear();
        }
        if (st == SEC_E_OK) break;
        if (st == SEC_I_CONTINUE_NEEDED) {
            need_read = t.in.empty();
            continue;
        }
        if (st == SEC_I_INCOMPLETE_CREDENTIALS) error = "the server asked for a client certificate";
        else error = sec_text(st);
        return false;
    }
    st = QueryContextAttributesA(&t.ctx, SECPKG_ATTR_STREAM_SIZES, &t.sizes);
    if (st != SEC_E_OK) {
        error = sec_text(st);
        return false;
    }
    return true;
}

bool tls_send(SOCKET s, Tls& t, const char* data, size_t n) {
    std::vector<char> buf(t.sizes.cbHeader + t.sizes.cbMaximumMessage + t.sizes.cbTrailer);
    while (n > 0) {
        size_t chunk = std::min<size_t>(n, t.sizes.cbMaximumMessage);
        std::memcpy(buf.data() + t.sizes.cbHeader, data, chunk);
        SecBuffer b[4] = {
            {t.sizes.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()},
            {(ULONG)chunk, SECBUFFER_DATA, buf.data() + t.sizes.cbHeader},
            {t.sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + t.sizes.cbHeader + chunk},
            {0, SECBUFFER_EMPTY, nullptr},
        };
        SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&t.ctx, 0, &d, 0) != SEC_E_OK) return false;
        if (!send_all(s, buf.data(), (size_t)b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return false;
        data += chunk;
        n -= chunk;
    }
    return true;
}

// 1: ok (maybe more data needed), 0: server closed the TLS session, -1: error.
int tls_decrypt(Tls& t, std::string& plain) {
    for (;;) {
        if (t.in.empty()) return 1;
        SecBuffer b[4] = {{(ULONG)t.in.size(), SECBUFFER_DATA, t.in.data()},
                          {0, SECBUFFER_EMPTY, nullptr},
                          {0, SECBUFFER_EMPTY, nullptr},
                          {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
        SECURITY_STATUS st = DecryptMessage(&t.ctx, &d, 0, nullptr);
        if (st == SEC_E_INCOMPLETE_MESSAGE) return 1;
        if (st == SEC_I_CONTEXT_EXPIRED) return 0;
        if (st != SEC_E_OK) return -1;   // includes renegotiation, which RTMP ingest never asks for
        std::vector<char> extra;
        for (auto& x : b) {
            if (x.BufferType == SECBUFFER_DATA && x.cbBuffer) plain.append((const char*)x.pvBuffer, x.cbBuffer);
            if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer)
                extra.assign((const char*)x.pvBuffer, (const char*)x.pvBuffer + x.cbBuffer);
        }
        t.in.swap(extra);
    }
}

} // namespace

TlsRelay::~TlsRelay() {
    stop();
}

std::string TlsRelay::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool TlsRelay::start(const std::string& host, int port, std::string& error) {
    ensure_winsock();
    host_ = host;
    port_ = port;
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == INVALID_SOCKET) {
        error = "Could not open a local socket for RTMPS.";
        return false;
    }
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    int len = sizeof(a);
    if (bind(l, (sockaddr*)&a, sizeof(a)) != 0 || listen(l, 1) != 0 || getsockname(l, (sockaddr*)&a, &len) != 0) {
        closesocket(l);
        error = "Could not open a local socket for RTMPS.";
        return false;
    }
    listener_ = (uintptr_t)l;
    local_port_ = ntohs(a.sin_port);
    stop_ = false;
    thread_ = std::thread(&TlsRelay::run, this);
    return true;
}

void TlsRelay::stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (listener_ != ~(uintptr_t)0) {
        closesocket((SOCKET)listener_);
        listener_ = ~(uintptr_t)0;
    }
}

void TlsRelay::run() {
    SOCKET l = (SOCKET)listener_;
    SOCKET client = INVALID_SOCKET;
    // Wait for librtmp (bounded, so a destination that never connects frees the thread).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!stop_ && std::chrono::steady_clock::now() < deadline) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(l, &r);
        timeval tv = {0, 200000};
        if (select(0, &r, nullptr, nullptr, &tv) > 0) {
            client = accept(l, nullptr, nullptr);
            break;
        }
    }
    closesocket(l);
    listener_ = ~(uintptr_t)0;
    if (client == INVALID_SOCKET) return;

    std::string err;
    SOCKET server = tcp_connect(host_, port_, err);
    Tls tls;
    if (server == INVALID_SOCKET || !tls_handshake(server, host_, tls, err)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "Could not connect securely to " + host_ + ": " + err + ".";
        if (server != INVALID_SOCKET) closesocket(server);
        closesocket(client);
        return;
    }

    std::vector<char> buf(16384);
    bool open = true;
    while (open && !stop_) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(client, &r);
        FD_SET(server, &r);
        timeval tv = {0, 200000};
        int n = select(0, &r, nullptr, nullptr, &tv);
        if (n < 0) break;
        if (n == 0) continue;
        if (FD_ISSET(client, &r)) {
            int k = recv(client, buf.data(), (int)buf.size(), 0);
            if (k <= 0 || !tls_send(server, tls, buf.data(), (size_t)k)) open = false;
        }
        if (open && FD_ISSET(server, &r)) {
            if (!recv_more(server, tls.in)) {
                open = false;
            } else {
                std::string plain;
                int rc = tls_decrypt(tls, plain);
                if (!plain.empty() && !send_all(client, plain.data(), plain.size())) open = false;
                if (rc <= 0) open = false;
            }
        }
    }
    shutdown(client, SD_BOTH);
    closesocket(client);
    closesocket(server);
}

bool tls_probe(const std::string& host, int port, std::string& error) {
    ensure_winsock();
    SOCKET s = tcp_connect(host, port, error);
    if (s == INVALID_SOCKET) return false;
    Tls tls;
    bool ok = tls_handshake(s, host, tls, error);
    closesocket(s);
    return ok;
}

} // namespace glint
