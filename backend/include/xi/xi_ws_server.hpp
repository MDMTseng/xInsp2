#pragma once
//
// xi_ws_server.hpp — minimal single-client WebSocket server.
//
// Header-only, Windows-only (uses Winsock2). Zero external deps beyond the
// system's schannel-less crypto (we bundle a tiny SHA1 for the handshake).
//
// Scope: localhost, single client at a time, text + binary frames up to
// 64 MiB, RFC 6455 handshake. No TLS, no extensions, no fragmentation on
// send (we always send whole messages in one frame). This is exactly what
// xInsp2 needs and nothing more.
//
// Usage:
//
//   xi::ws::Server srv;
//   srv.on_text   = [](std::string_view s) { ... };
//   srv.on_binary = [](const uint8_t* p, size_t n) { ... };
//   srv.on_open   = [] { ... };
//   srv.on_close  = [] { ... };
//   srv.start(7823);
//   while (srv.is_running()) srv.poll(100);   // 100ms poll interval
//
// Outbound sends are thread-safe (guarded by a mutex); inbound callbacks
// fire on the thread that called poll().
//

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #define _WINSOCK_DEPRECATED_NO_WARNINGS
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
  using socket_t = SOCKET;
  static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
  #define CLOSESOCK closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t INVALID_SOCK = -1;
  #define CLOSESOCK ::close
#endif

#include "xi_sha256.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xi::ws {

// ---------- tiny SHA1 (public-domain Steve Reid impl, trimmed) ----------
namespace detail {

struct Sha1 {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buffer[64];
};

inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

inline void sha1_transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e;
    uint32_t block[16];
    for (int i = 0; i < 16; ++i) {
        block[i] = (uint32_t)buffer[i * 4 + 0] << 24 |
                   (uint32_t)buffer[i * 4 + 1] << 16 |
                   (uint32_t)buffer[i * 4 + 2] << 8  |
                   (uint32_t)buffer[i * 4 + 3];
    }
#define BLK(i) (block[i & 15] = rol(block[(i + 13) & 15] ^ block[(i + 8) & 15] ^ block[(i + 2) & 15] ^ block[i & 15], 1))
#define R0(v, w, x, y, z, i) z += ((w & (x ^ y)) ^ y) + block[i] + 0x5A827999 + rol(v, 5); w = rol(w, 30);
#define R1(v, w, x, y, z, i) z += ((w & (x ^ y)) ^ y) + BLK(i)   + 0x5A827999 + rol(v, 5); w = rol(w, 30);
#define R2(v, w, x, y, z, i) z += (w ^ x ^ y)        + BLK(i)   + 0x6ED9EBA1 + rol(v, 5); w = rol(w, 30);
#define R3(v, w, x, y, z, i) z += (((w | x) & y) | (w & x)) + BLK(i) + 0x8F1BBCDC + rol(v, 5); w = rol(w, 30);
#define R4(v, w, x, y, z, i) z += (w ^ x ^ y)        + BLK(i)   + 0xCA62C1D6 + rol(v, 5); w = rol(w, 30);

    a = state[0]; b = state[1]; c = state[2]; d = state[3]; e = state[4];

    R0(a,b,c,d,e, 0); R0(e,a,b,c,d, 1); R0(d,e,a,b,c, 2); R0(c,d,e,a,b, 3);
    R0(b,c,d,e,a, 4); R0(a,b,c,d,e, 5); R0(e,a,b,c,d, 6); R0(d,e,a,b,c, 7);
    R0(c,d,e,a,b, 8); R0(b,c,d,e,a, 9); R0(a,b,c,d,e,10); R0(e,a,b,c,d,11);
    R0(d,e,a,b,c,12); R0(c,d,e,a,b,13); R0(b,c,d,e,a,14); R0(a,b,c,d,e,15);
    R1(e,a,b,c,d,16); R1(d,e,a,b,c,17); R1(c,d,e,a,b,18); R1(b,c,d,e,a,19);
    R2(a,b,c,d,e,20); R2(e,a,b,c,d,21); R2(d,e,a,b,c,22); R2(c,d,e,a,b,23);
    R2(b,c,d,e,a,24); R2(a,b,c,d,e,25); R2(e,a,b,c,d,26); R2(d,e,a,b,c,27);
    R2(c,d,e,a,b,28); R2(b,c,d,e,a,29); R2(a,b,c,d,e,30); R2(e,a,b,c,d,31);
    R2(d,e,a,b,c,32); R2(c,d,e,a,b,33); R2(b,c,d,e,a,34); R2(a,b,c,d,e,35);
    R2(e,a,b,c,d,36); R2(d,e,a,b,c,37); R2(c,d,e,a,b,38); R2(b,c,d,e,a,39);
    R3(a,b,c,d,e,40); R3(e,a,b,c,d,41); R3(d,e,a,b,c,42); R3(c,d,e,a,b,43);
    R3(b,c,d,e,a,44); R3(a,b,c,d,e,45); R3(e,a,b,c,d,46); R3(d,e,a,b,c,47);
    R3(c,d,e,a,b,48); R3(b,c,d,e,a,49); R3(a,b,c,d,e,50); R3(e,a,b,c,d,51);
    R3(d,e,a,b,c,52); R3(c,d,e,a,b,53); R3(b,c,d,e,a,54); R3(a,b,c,d,e,55);
    R3(e,a,b,c,d,56); R3(d,e,a,b,c,57); R3(c,d,e,a,b,58); R3(b,c,d,e,a,59);
    R4(a,b,c,d,e,60); R4(e,a,b,c,d,61); R4(d,e,a,b,c,62); R4(c,d,e,a,b,63);
    R4(b,c,d,e,a,64); R4(a,b,c,d,e,65); R4(e,a,b,c,d,66); R4(d,e,a,b,c,67);
    R4(c,d,e,a,b,68); R4(b,c,d,e,a,69); R4(a,b,c,d,e,70); R4(e,a,b,c,d,71);
    R4(d,e,a,b,c,72); R4(c,d,e,a,b,73); R4(b,c,d,e,a,74); R4(a,b,c,d,e,75);
    R4(e,a,b,c,d,76); R4(d,e,a,b,c,77); R4(c,d,e,a,b,78); R4(b,c,d,e,a,79);

    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;

#undef BLK
#undef R0
#undef R1
#undef R2
#undef R3
#undef R4
}

inline void sha1_init(Sha1& c) {
    c.state[0] = 0x67452301;
    c.state[1] = 0xEFCDAB89;
    c.state[2] = 0x98BADCFE;
    c.state[3] = 0x10325476;
    c.state[4] = 0xC3D2E1F0;
    c.count[0] = c.count[1] = 0;
}

inline void sha1_update(Sha1& c, const uint8_t* data, size_t len) {
    uint32_t i, j;
    j = c.count[0];
    if ((c.count[0] += (uint32_t)(len << 3)) < j) c.count[1]++;
    c.count[1] += (uint32_t)(len >> 29);
    j = (j >> 3) & 63;
    if ((j + len) > 63) {
        std::memcpy(&c.buffer[j], data, (i = 64 - j));
        sha1_transform(c.state, c.buffer);
        for (; i + 63 < len; i += 64) sha1_transform(c.state, &data[i]);
        j = 0;
    } else {
        i = 0;
    }
    std::memcpy(&c.buffer[j], &data[i], len - i);
}

inline void sha1_final(Sha1& c, uint8_t digest[20]) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; ++i) {
        finalcount[i] = (uint8_t)((c.count[(i >= 4 ? 0 : 1)] >> ((3 - (i & 3)) * 8)) & 255);
    }
    uint8_t c0 = 0x80;
    sha1_update(c, &c0, 1);
    while ((c.count[0] & 504) != 448) {
        c0 = 0;
        sha1_update(c, &c0, 1);
    }
    sha1_update(c, finalcount, 8);
    for (int i = 0; i < 20; ++i) {
        digest[i] = (uint8_t)((c.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 255);
    }
}

inline std::string sha1(std::string_view s) {
    Sha1 c;
    sha1_init(c);
    sha1_update(c, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    uint8_t digest[20];
    sha1_final(c, digest);
    return std::string(reinterpret_cast<char*>(digest), 20);
}

// base64 encode (fixed 20-byte input is all we need).
inline std::string base64(const uint8_t* in, size_t n) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= (uint32_t)in[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(i + 1 < n ? tbl[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < n ? tbl[v & 63]        : '=');
    }
    return out;
}

inline std::string ws_accept_key(std::string_view sec_key) {
    std::string s = std::string(sec_key) + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    auto digest = sha1(s);
    return base64(reinterpret_cast<const uint8_t*>(digest.data()), digest.size());
}

inline std::string to_lower(std::string_view s) {
    std::string o(s);
    for (auto& c : o) c = (char)::tolower((unsigned char)c);
    return o;
}

} // namespace detail

// ---------- Server ----------

class Server {
public:
    std::function<void()>                            on_open;
    std::function<void()>                            on_close;
    std::function<void(std::string_view)>            on_text;
    std::function<void(const uint8_t*, size_t)>      on_binary;

    Server() = default;
    ~Server() { stop(); }

    // Bind address. Accepts "127.0.0.1" (loopback, default), "0.0.0.0"
    // (all interfaces), or a specific IPv4 literal. Call before start().
    void set_bind_host(std::string host) { bind_host_ = std::move(host); }

    // Optional shared-secret for remote mode. When non-empty, the
    // WebSocket handshake must carry `Authorization: Bearer <secret>`;
    // anything else gets a 401 and is disconnected. Ignored when empty
    // (default) so localhost-only deployments stay friction-free.
    void set_auth_secret(std::string secret) { auth_secret_ = std::move(secret); }

    bool start(int port) {
#ifdef _WIN32
        static bool wsa_inited = false;
        if (!wsa_inited) {
            WSADATA d;
            if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
            wsa_inited = true;
        }
#endif
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ == INVALID_SOCK) return false;
        int opt = 1;
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        // Resolve bind host. Empty / "127.0.0.1" → loopback, "0.0.0.0"
        // → INADDR_ANY, else inet_addr().
        if (bind_host_.empty() || bind_host_ == "127.0.0.1" || bind_host_ == "localhost") {
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        } else if (bind_host_ == "0.0.0.0") {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            unsigned long ip = ::inet_addr(bind_host_.c_str());
            if (ip == INADDR_NONE) {
                CLOSESOCK(listen_);
                listen_ = INVALID_SOCK;
                return false;
            }
            addr.sin_addr.s_addr = ip;
        }
        addr.sin_port        = htons((u_short)port);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            CLOSESOCK(listen_);
            listen_ = INVALID_SOCK;
            return false;
        }
        if (::listen(listen_, 1) != 0) {
            CLOSESOCK(listen_);
            listen_ = INVALID_SOCK;
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() {
        running_ = false;
        close_client();
        if (listen_ != INVALID_SOCK) {
            CLOSESOCK(listen_);
            listen_ = INVALID_SOCK;
        }
    }

    bool is_running() const { return running_; }
    bool has_client() const { return client_ != INVALID_SOCK; }

    // Blocks up to timeout_ms for activity. Accepts a new client, performs
    // the handshake, and reads any pending frames, dispatching callbacks.
    void poll(int timeout_ms) {
        if (!running_) return;

        fd_set rfds;
        FD_ZERO(&rfds);
        socket_t maxfd = 0;
        if (listen_ != INVALID_SOCK && client_ == INVALID_SOCK) {
            FD_SET(listen_, &rfds);
            maxfd = std::max(maxfd, listen_);
        }
        if (client_ != INVALID_SOCK) {
            FD_SET(client_, &rfds);
            maxfd = std::max(maxfd, client_);
        }

        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int n = ::select((int)(maxfd + 1), &rfds, nullptr, nullptr, &tv);
        if (n <= 0) return;

        if (listen_ != INVALID_SOCK && FD_ISSET(listen_, &rfds) && client_ == INVALID_SOCK) {
            sockaddr_in peer{};
            socklen_t peerlen = sizeof(peer);
            socket_t s = ::accept(listen_, reinterpret_cast<sockaddr*>(&peer), &peerlen);
            if (s != INVALID_SOCK) {
                if (do_handshake(s)) {
                    client_ = s;
                    if (on_open) on_open();
                } else {
                    CLOSESOCK(s);
                }
            }
        }

        if (client_ != INVALID_SOCK && FD_ISSET(client_, &rfds)) {
            if (!read_pending()) {
                close_client();
                if (on_close) on_close();
            }
        }
    }

    // Thread-safe send. Returns false if no client is connected or the
    // send fails.
    bool send_text(std::string_view s) {
        return send_frame(0x1, reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    bool send_binary(const uint8_t* data, size_t n) {
        return send_frame(0x2, data, n);
    }

private:
    // Upper bound on a single WebSocket frame payload and on the total
    // size of a reassembled fragmented message. 16 MiB comfortably covers
    // JPEG previews of 20 MP frames (typical 300 KB–2 MB) while keeping
    // the memory blast radius small for a localhost control channel.
    static constexpr size_t kMaxFrame   = 16u * 1024u * 1024u;
    static constexpr size_t kMaxMessage = 16u * 1024u * 1024u;

    socket_t    listen_ = INVALID_SOCK;
    socket_t    client_ = INVALID_SOCK;
    bool        running_ = false;
    std::vector<uint8_t> rx_buf_;
    std::mutex  tx_mu_;

    // Fragmented-message reassembly state (RFC 6455 §5.4). When a data
    // frame arrives with fin=0 we stash its opcode and payload; subsequent
    // continuation frames (opcode 0x0) append until fin=1.
    std::vector<uint8_t> msg_buf_;
    int                  msg_opcode_     = 0;
    bool                 msg_in_progress_ = false;

    std::string          bind_host_;     // empty/127.0.0.1 = loopback
    std::string          auth_secret_;   // empty = no auth required

    void close_client() {
        if (client_ != INVALID_SOCK) {
            CLOSESOCK(client_);
            client_ = INVALID_SOCK;
            rx_buf_.clear();
            msg_buf_.clear();
            msg_buf_.shrink_to_fit();
            msg_in_progress_ = false;
            msg_opcode_      = 0;
        }
    }

    // Read HTTP request, parse Sec-WebSocket-Key, send 101 switching.
    // Any trailing bytes after the \r\n\r\n (a pipelined first frame) are
    // preserved in rx_buf_ so read_pending() can consume them on the next
    // poll cycle.
    bool do_handshake(socket_t s) {
        std::string req;
        char buf[2048];
        size_t header_end = std::string::npos;
        for (int i = 0; i < 16; ++i) {
            int n = ::recv(s, buf, (int)sizeof(buf), 0);
            if (n <= 0) return false;
            req.append(buf, buf + n);
            header_end = req.find("\r\n\r\n");
            if (header_end != std::string::npos) break;
        }
        if (header_end == std::string::npos) return false;

        auto lc = detail::to_lower(req);
        auto key_pos = lc.find("sec-websocket-key:");
        if (key_pos == std::string::npos) return false;
        key_pos += std::strlen("sec-websocket-key:");
        while (key_pos < req.size() && (req[key_pos] == ' ' || req[key_pos] == '\t')) ++key_pos;
        auto eol = req.find("\r\n", key_pos);
        if (eol == std::string::npos) return false;
        std::string key = req.substr(key_pos, eol - key_pos);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\r' || key.back() == '\t')) key.pop_back();

        // Optional shared-secret auth. Two modes:
        //
        // 1. Plain bearer (legacy): auth_secret is the secret itself.
        //    Client sends `Authorization: Bearer <secret>`. Anyone
        //    sniffing the handshake can replay forever — only use on
        //    trusted networks (loopback / VPN).
        //
        // 2. HMAC challenge (auth_secret starts with "hmac:"): the
        //    rest of auth_secret is the HMAC key. Client sends
        //    `X-Xi-Timestamp: <unix_seconds>` and
        //    `Authorization: Bearer <hex(hmac_sha256(key, ts))>`.
        //    Server verifies the timestamp is within ±60 s of now AND
        //    the HMAC matches. Replay window is 60 s instead of
        //    forever — but the WS frames after handshake are still
        //    plaintext, so for hostile networks use a TLS reverse
        //    proxy (nginx) in front of this. HMAC mode is a layered
        //    defence, not a substitute for TLS.
        if (!auth_secret_.empty()) {
            const std::string hmac_prefix = "hmac:";
            bool hmac_mode = auth_secret_.compare(0, hmac_prefix.size(), hmac_prefix) == 0;
            std::string hmac_key = hmac_mode
                ? auth_secret_.substr(hmac_prefix.size())
                : std::string{};
            const std::string tag = "authorization:";
            auto a_pos = lc.find(tag);
            bool ok = false;
            if (a_pos != std::string::npos) {
                a_pos += tag.size();
                while (a_pos < req.size() && (req[a_pos] == ' ' || req[a_pos] == '\t')) ++a_pos;
                auto a_eol = req.find("\r\n", a_pos);
                if (a_eol != std::string::npos) {
                    std::string hdr = req.substr(a_pos, a_eol - a_pos);
                    while (!hdr.empty() && (hdr.back() == ' ' || hdr.back() == '\r' || hdr.back() == '\t')) hdr.pop_back();
                    const std::string prefix = "Bearer ";
                    if (hdr.size() > prefix.size() &&
                        hdr.compare(0, prefix.size(), prefix) == 0) {
                        std::string_view got(hdr.data() + prefix.size(),
                                             hdr.size() - prefix.size());
                        if (hmac_mode) {
                            // Pull X-Xi-Timestamp header.
                            const std::string ts_tag = "x-xi-timestamp:";
                            auto ts_pos = lc.find(ts_tag);
                            if (ts_pos != std::string::npos) {
                                ts_pos += ts_tag.size();
                                while (ts_pos < req.size() && (req[ts_pos] == ' ' || req[ts_pos] == '\t')) ++ts_pos;
                                auto ts_eol = req.find("\r\n", ts_pos);
                                if (ts_eol != std::string::npos) {
                                    std::string ts_str = req.substr(ts_pos, ts_eol - ts_pos);
                                    while (!ts_str.empty() && (ts_str.back() == ' ' || ts_str.back() == '\r' || ts_str.back() == '\t')) ts_str.pop_back();
                                    int64_t ts = 0;
                                    try { ts = std::stoll(ts_str); } catch (...) {}
                                    int64_t now = (int64_t)std::time(nullptr);
                                    if (ts != 0 && std::abs(now - ts) <= 60) {
                                        std::string expected =
                                            xi::sha256::hmac_sha256(hmac_key, ts_str);
                                        if (got.size() == expected.size()) {
                                            unsigned diff = 0;
                                            for (size_t i = 0; i < got.size(); ++i)
                                                diff |= (unsigned char)got[i] ^ (unsigned char)expected[i];
                                            ok = (diff == 0);
                                        }
                                    }
                                }
                            }
                        } else if (got.size() == auth_secret_.size()) {
                            unsigned diff = 0;
                            for (size_t i = 0; i < got.size(); ++i)
                                diff |= (unsigned char)got[i] ^ (unsigned char)auth_secret_[i];
                            ok = (diff == 0);
                        }
                    }
                }
            }
            if (!ok) {
                const char* deny =
                    "HTTP/1.1 401 Unauthorized\r\n"
                    "Content-Length: 0\r\n"
                    "WWW-Authenticate: Bearer\r\n"
                    "\r\n";
                ::send(s, deny, (int)std::strlen(deny), 0);
                return false;
            }
        }

        std::string accept = detail::ws_accept_key(key);
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n"
            "\r\n";
        int sent = ::send(s, resp.data(), (int)resp.size(), 0);
        if (sent != (int)resp.size()) return false;

        // Preserve anything we accidentally slurped past \r\n\r\n.
        size_t body_start = header_end + 4;
        if (body_start < req.size()) {
            rx_buf_.insert(rx_buf_.end(),
                reinterpret_cast<const uint8_t*>(req.data() + body_start),
                reinterpret_cast<const uint8_t*>(req.data() + req.size()));
        }
        return true;
    }

    bool read_pending() {
        uint8_t tmp[16384];
        int n = ::recv(client_, reinterpret_cast<char*>(tmp), (int)sizeof(tmp), 0);
        if (n <= 0) return false;
        rx_buf_.insert(rx_buf_.end(), tmp, tmp + n);
        // Guard against unbounded growth: if we've buffered more than a
        // legal frame's worth without finding a frame boundary, the peer
        // is either broken or malicious. Drop.
        if (rx_buf_.size() > kMaxFrame + 16) return false;

        // Parse as many frames as present
        for (;;) {
            size_t consumed = 0;
            int    op       = 0;
            bool   fin      = false;
            std::vector<uint8_t> payload;
            auto r = parse_frame(rx_buf_.data(), rx_buf_.size(), op, fin, payload, consumed);
            if (r == ParseResult::Need) break;
            if (r == ParseResult::Bad)  return false;
            rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + consumed);

            // Control frames (opcode ≥ 0x8) must not be fragmented and
            // must carry ≤125 bytes (RFC 6455 §5.5).
            const bool is_control = (op & 0x8) != 0;
            if (is_control && (!fin || payload.size() > 125)) return false;

            if (op == 0x8) return false; // close
            if (op == 0x9) {
                // ping -> pong
                send_frame(0xA, payload.data(), payload.size());
                continue;
            }
            if (op == 0xA) continue; // pong ignored

            // Data frames: handle continuation / fragmentation.
            if (op == 0x0) {
                // Continuation frame.
                if (!msg_in_progress_) return false; // protocol error
                if (msg_buf_.size() + payload.size() > kMaxMessage) return false;
                msg_buf_.insert(msg_buf_.end(), payload.begin(), payload.end());
            } else if (op == 0x1 || op == 0x2) {
                // Start of a new data message.
                if (msg_in_progress_) return false; // missing continuation
                msg_opcode_      = op;
                msg_in_progress_ = true;
                if (payload.size() > kMaxMessage) return false;
                msg_buf_ = std::move(payload);
            } else {
                // Unknown opcode.
                return false;
            }

            if (fin) {
                if (msg_opcode_ == 0x1 && on_text) {
                    on_text(std::string_view(reinterpret_cast<const char*>(msg_buf_.data()),
                                             msg_buf_.size()));
                } else if (msg_opcode_ == 0x2 && on_binary) {
                    on_binary(msg_buf_.data(), msg_buf_.size());
                }
                msg_buf_.clear();
                msg_buf_.shrink_to_fit();
                msg_in_progress_ = false;
                msg_opcode_      = 0;
            }
        }
        return true;
    }

    enum class ParseResult { Ok, Need, Bad };

    ParseResult parse_frame(const uint8_t* p, size_t n,
                            int& op_out, bool& fin_out,
                            std::vector<uint8_t>& payload,
                            size_t& consumed) {
        if (n < 2) return ParseResult::Need;
        uint8_t b0 = p[0];
        uint8_t b1 = p[1];
        bool    fin    = (b0 & 0x80) != 0;
        int     opcode = b0 & 0x0F;
        bool    masked = (b1 & 0x80) != 0;
        uint64_t len   = b1 & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (n < hdr + 2) return ParseResult::Need;
            len = ((uint64_t)p[hdr] << 8) | p[hdr + 1];
            hdr += 2;
        } else if (len == 127) {
            if (n < hdr + 8) return ParseResult::Need;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[hdr + i];
            hdr += 8;
        }
        // Reject oversized frames before any allocation happens.
        if (len > kMaxFrame) return ParseResult::Bad;
        if (!masked) return ParseResult::Bad; // clients must mask
        if (n < hdr + 4 + len) return ParseResult::Need;
        uint8_t mask[4] = { p[hdr], p[hdr + 1], p[hdr + 2], p[hdr + 3] };
        hdr += 4;
        payload.resize(len);
        for (uint64_t i = 0; i < len; ++i) {
            payload[i] = p[hdr + i] ^ mask[i & 3];
        }
        consumed = hdr + len;
        op_out   = opcode;
        fin_out  = fin;
        return ParseResult::Ok;
    }

    bool send_frame(int opcode, const uint8_t* data, size_t n) {
        std::lock_guard<std::mutex> g(tx_mu_);
        if (client_ == INVALID_SOCK) return false;
        uint8_t hdr[10];
        size_t  hlen = 0;
        hdr[0] = 0x80 | (uint8_t)opcode;
        if (n < 126) {
            hdr[1] = (uint8_t)n;
            hlen = 2;
        } else if (n <= 0xFFFF) {
            hdr[1] = 126;
            hdr[2] = (uint8_t)((n >> 8) & 0xFF);
            hdr[3] = (uint8_t)(n & 0xFF);
            hlen = 4;
        } else {
            hdr[1] = 127;
            uint64_t v = n;
            for (int i = 0; i < 8; ++i) hdr[2 + i] = (uint8_t)((v >> ((7 - i) * 8)) & 0xFF);
            hlen = 10;
        }
        int s1 = ::send(client_, reinterpret_cast<const char*>(hdr), (int)hlen, 0);
        if (s1 != (int)hlen) return false;
        if (n == 0) return true;
        size_t sent = 0;
        while (sent < n) {
            int chunk = (int)std::min<size_t>(n - sent, 1 << 20);
            int s2 = ::send(client_, reinterpret_cast<const char*>(data + sent), chunk, 0);
            if (s2 <= 0) return false;
            sent += (size_t)s2;
        }
        return true;
    }
};

} // namespace xi::ws
