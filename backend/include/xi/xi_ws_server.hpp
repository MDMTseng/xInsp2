#pragma once
//
// xi_ws_server.hpp — minimal single-client WebSocket server.
//
// Header-only, Windows-only (uses Winsock2). Zero external deps beyond the
// system's schannel-less crypto (we bundle a tiny SHA1 for the handshake).
//
// Scope: localhost, single client at a time, text + binary frames up to
// 16 MiB (kMaxFrame/kMaxMessage), RFC 6455 handshake. No TLS, no extensions, no fragmentation on
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
// Outbound sends are thread-safe AND non-blocking: send_text/send_binary build
// the whole WS frame into one owned buffer and ENQUEUE it on an ordered outbound
// queue, then return. A single long-lived writer thread (owned by the Server,
// spawned in start(), joined in stop()) drains that queue FIFO and does the only
// blocking ::send. This decouples the caller (dispatch worker inside the
// ordered-emit gate) from the client's socket drain rate: a slow-but-alive WS
// client can no longer pin the whole ordered inspection lane on its network I/O
// (red-team RT8 / doc 21 §P2 — see docs/new_gen/23-rt8-async-writer.md). Inbound
// callbacks still fire on the thread that called poll().
//
// send_frame therefore returns true on ENQUEUE, not on delivery (a WS send was
// never a delivery guarantee — TCP buffering already meant sent != received). It
// returns false only when there is NO client, or the outbound byte budget
// (kOutboundHardCapBytes) is exceeded and the wedged client is dropped.
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
  #include <fcntl.h>        // O_NONBLOCK — the busy-reject drain (see poll())
  #include <netinet/in.h>
  #include <netinet/tcp.h>   // TCP_NODELAY
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t INVALID_SOCK = -1;
  #define CLOSESOCK ::close
#endif

#include "xi_b64.hpp"
#include "xi_sha256.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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

// base64 encode (fixed 20-byte input is all we need) — one-line forwarder to
// the shared SDK leaf (xi_b64.hpp); the local name is kept for call sites.
inline std::string base64(const uint8_t* in, size_t n) { return xi::b64_encode(in, n); }

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

// Round-3 W2 #1: the ONE handshake header extractor. Root cause: the three
// headers the handshake reads (Sec-WebSocket-Key / Authorization /
// X-Xi-Timestamp) each hand-rolled the same find-in-lowercased-request →
// skip OWS → find CRLF → substr → rtrim ritual, and the trim sets were one
// drifted copy away from diverging. `lc` is detail::to_lower(req) (computed
// once by the caller); `name_lc` is the lowercase header name INCLUDING the
// trailing ':'. Leading skip is space/tab (RFC 7230 OWS); the trailing trim
// is the unified, most-correct variant all three copies already agreed on —
// space/tab (OWS) plus a defensive '\r' (unreachable while the value ends at
// the found CRLF, kept so a future caller passing a pre-split line can't
// leak a stray CR). Returns nullopt when the header is absent or unterminated.
inline std::optional<std::string> get_header_(const std::string& lc,
                                              const std::string& req,
                                              const char* name_lc) {
    size_t pos = lc.find(name_lc);
    if (pos == std::string::npos) return std::nullopt;
    pos += std::strlen(name_lc);
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) ++pos;
    size_t eol = req.find("\r\n", pos);
    if (eol == std::string::npos) return std::nullopt;
    std::string v = req.substr(pos, eol - pos);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\t'))
        v.pop_back();
    return v;
}

// Round-3 W2 #2: constant-time equality for handshake credentials. Root
// cause: the HMAC digest check and the plain bearer check carried identical
// inline XOR-fold loops — the one comparison in the tree that must NOT be a
// drifting copy (a "fixed" early-exit variant would reintroduce the timing
// side channel). Length mismatch returns false immediately: the length of
// the expected credential is not secret, only its bytes are.
inline bool ct_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
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
#ifdef _WIN32
        // SO_EXCLUSIVEADDRUSE, NOT SO_REUSEADDR, on the Windows listen socket.
        // On Windows SO_REUSEADDR does NOT mean what it means on POSIX: it
        // permits a *full duplicate active bind* — any other local process can
        // ::bind() our exact ip:port while we are listening, and the kernel then
        // splits inbound connections between the two rival listeners
        // nondeterministically (connection hijack; demonstrated live in the
        // ws_teardown_race deflake — two listeners on port 39187, each getting a
        // share of connects, the starved one seeing opens=0). SO_EXCLUSIVEADDRUSE
        // makes our bind exclusive so no rival can steal the port. It does NOT
        // reintroduce Linux-style TIME_WAIT rebind pain: on Windows a *listen*
        // socket in the normal close path frees the port immediately for the same
        // account, so the FE's rapid BE respawn (close listener → new BE binds
        // same fixed port) still works — covered by the respawn-cycle test.
        ::setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        // POSIX SO_REUSEADDR is the safe, needed one: it does NOT allow a second
        // active bind on the same ip:port while we are listening (that still
        // fails with EADDRINUSE), it only lets us rebind a port stuck in
        // TIME_WAIT from a prior socket — exactly the rapid-respawn case. Keep it.
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif
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
        // Backlog: although we only serve one client at a time, the OS-level
        // listen backlog buffers SYNs that arrive while we're between accept()
        // calls (e.g. select() yet to fire, or poll() blocked in read_pending).
        // backlog=1 caused FL r7 P1: under sustained malformed input, fresh
        // probe connections received WSAECONNREFUSED for 1-2 s every 50-200
        // iters because the kernel had nowhere to queue the new SYN. SOMAXCONN
        // costs nothing extra on Windows (clamped internally) and gives us
        // headroom for transient bursts.
        if (::listen(listen_, SOMAXCONN) != 0) {
            CLOSESOCK(listen_);
            listen_ = INVALID_SOCK;
            return false;
        }
        // Capture the actual bound port. When the caller passes port 0 the OS
        // assigns an ephemeral one; getsockname reads it back so callers can
        // discover where to connect (see local_port()). For a fixed non-zero
        // port this simply echoes it.
        sockaddr_in bound{};
        socklen_t bound_len = sizeof(bound);
        if (::getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
            local_port_ = ntohs(bound.sin_port);
        } else {
            local_port_ = port;
        }
        running_ = true;
        // Spawn the ordered outbound writer AFTER the listen socket is up. It is
        // the ONLY thread the Server owns (the poll loop is driven externally by
        // the service main loop). It drains out_q_ FIFO and does the sole
        // blocking ::send — see writer_loop_.
        writer_stop_ = false;
        writer_thread_ = std::thread(&Server::writer_loop_, this);
        return true;
    }

    // Idempotent — safe to call twice (stop() may be invoked explicitly AND again
    // from ~Server()). Order matters: signal + JOIN the writer FIRST (so nothing
    // is torn down out from under a running writer), THEN close the client and the
    // listen socket. The join is bounded: a writer blocked in ::send is unblocked
    // by its own SO_SNDTIMEO (<=1.5s) at worst, after which it observes the stop
    // flag and exits.
    void stop() {
        running_ = false;
        // 1) Wake + join the writer. Set the stop flag under out_mu_ so a writer
        //    parked in out_cv_.wait sees it, then notify. joinable() guards against
        //    a double-join on the second stop() call.
        {
            std::lock_guard<std::mutex> g(out_mu_);
            writer_stop_ = true;
        }
        out_cv_.notify_all();
        // G5: unblock a writer parked in ::send() to a slow-but-PROGRESSING client
        // BEFORE joining. SO_SNDTIMEO only catches a fully-wedged 0-drain peer; a
        // client draining each 1 MiB chunk in <1.5s never trips it, so join() would
        // otherwise wait out the whole in-flight frame (a 16 MiB preview at
        // ~500 KB/s ≈ 30s), blowing the ≤1.5s stop bound. shutdown() is the same
        // unblock close_client()/the writer error path use; full teardown still runs
        // in close_client() below.
        {
            socket_t fd = client_.load(std::memory_order_acquire);
            if (fd != INVALID_SOCK) ::shutdown(fd, 2 /* SD_BOTH / SHUT_RDWR */);
        }
        if (writer_thread_.joinable()) writer_thread_.join();
        // 2) Now that no writer is touching client_ or the queue, tear down the
        //    client (close_client also clears the — now unattended — queue).
        //    Round-3 W2 #3: every other client-death path (poll's proactive-drop
        //    honor, the read_pending failure) runs close_client()+on_close() as a
        //    PAIR; stop() was the lone asymmetric path, so a client dropped by
        //    shutdown never got its on_close bookkeeping. on_close fires on the
        //    stop() caller's thread — safe for the sole production consumer
        //    (service_main clears the recent-errors ring under its own mutex,
        //    and main() calls stop() after the poll loop exits while g_eng is
        //    still alive). Gated on a live client so the idempotent second
        //    stop() (~Server) cannot double-fire it.
        bool had_client = client_.load(std::memory_order_acquire) != INVALID_SOCK;
        close_client();
        if (had_client && on_close) on_close();
        if (listen_ != INVALID_SOCK) {
            CLOSESOCK(listen_);
            listen_ = INVALID_SOCK;
        }
        local_port_ = 0;
    }

    bool is_running() const { return running_; }
    bool has_client() const { return client_.load(std::memory_order_acquire) != INVALID_SOCK; }

    // The TCP port the listen socket is actually bound to. Meaningful after a
    // successful start(); equals the requested port, or the OS-assigned
    // ephemeral port when start(0) was used. 0 before start()/after a failed
    // start(). Lets tests bind an ephemeral port (start(0)) and then connect to
    // it, so they never collide on a shared fixed port.
    int local_port() const { return local_port_; }

    // Blocks up to timeout_ms for activity. Accepts a new client, performs
    // the handshake, and reads any pending frames, dispatching callbacks.
    void poll(int timeout_ms) {
        if (!running_) return;

        // Honor a proactive drop requested by the send side (byte-cap or a failed
        // writer ::send). close_client()+on_close run HERE, on the poll thread, so
        // the rx/msg buffers stay single-threaded and a silent wedged client that
        // never reacts to the FIN is still dropped within one poll tick.
        if (drop_requested_.exchange(false, std::memory_order_acq_rel)) {
            if (client_.load(std::memory_order_acquire) != INVALID_SOCK) {
                close_client();
                if (on_close) on_close();
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        socket_t maxfd = 0;
        // Always watch listen_ — even with a client connected. A 2nd client's
        // SYN that the kernel queued must reach accept() promptly; otherwise
        // it sits in the SYN queue until the OS times it out (~21 s on
        // Windows), which the caller perceives as connection-refused-after-
        // long-stall. We accept-and-reject immediately below.
        if (listen_ != INVALID_SOCK) {
            FD_SET(listen_, &rfds);
            maxfd = std::max(maxfd, listen_);
        }
        // Poll thread is the sole writer of client_, so this snapshot is stable
        // for the rest of poll(); the writer thread only reads it (under tx_mu_).
        socket_t cl = client_.load(std::memory_order_acquire);
        if (cl != INVALID_SOCK) {
            FD_SET(cl, &rfds);
            maxfd = std::max(maxfd, cl);
        }

        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int n = ::select((int)(maxfd + 1), &rfds, nullptr, nullptr, &tv);
        if (n <= 0) return;

        if (listen_ != INVALID_SOCK && FD_ISSET(listen_, &rfds)) {
            sockaddr_in peer{};
            socklen_t peerlen = sizeof(peer);
            socket_t s = ::accept(listen_, reinterpret_cast<sockaddr*>(&peer), &peerlen);
            if (s != INVALID_SOCK) {
                if (cl == INVALID_SOCK) {
                    if (do_handshake(s)) {
                        // Bound the SEND direction. Post-RT8 the blocking ::send
                        // runs ONLY on the Server's single writer thread (never the
                        // dispatch workers — they just enqueue). SO_SNDTIMEO still
                        // matters: it bounds the WRITER's ::send so a FULLY-wedged
                        // (0-drain) client — TCP window full, never draining — is
                        // noticed and dropped after a bounded wait instead of parking
                        // the writer forever. (A slow-but-progressing client that
                        // never trips this is instead caught by the outbound
                        // byte-cap, kOutboundHardCapBytes.)
                        const int kSendTimeoutMs = 1500;
#ifdef _WIN32
                        DWORD sto = kSendTimeoutMs;
                        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sto, sizeof(sto));
#else
                        struct timeval sto{}; sto.tv_sec = kSendTimeoutMs / 1000;
                        sto.tv_usec = (kSendTimeoutMs % 1000) * 1000;
                        ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));
#endif
                        // Large-frame egress tuning (bench5mp: a raw 5MP preview is
                        // ~15 MB/frame; the default 64 KB SNDBUF throttled the sole
                        // writer to ~350 MB/s on loopback). A 4 MiB SNDBUF lets the
                        // kernel stream ahead of the writer's chunk loop, and
                        // TCP_NODELAY removes the tail-segment Nagle stall on each
                        // enqueue boundary. Localhost-first numbers; both are also
                        // sane (or no-ops) for a LAN client.
                        // SIZE COUPLING (qa_slow_consumer phase 2): the kernel buffer
                        // silently absorbs a WEDGED client's backlog before ::send
                        // blocks, so SO_SNDTIMEO fires only after SNDBUF fills — the
                        // wedge-drop bound grows with SNDBUF/production-rate. 4 MiB
                        // measured the SAME throughput as 16 MiB (the producer is the
                        // ceiling past ~483 MB/s) while keeping the wedge fill time
                        // bounded for slow lanes; do NOT raise this without re-running
                        // qa_slow_consumer.
                        {
                            int sndbuf = 4 * 1024 * 1024;
                            ::setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                                         (const char*)&sndbuf, sizeof(sndbuf));
                            int nodelay = 1;
                            ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                                         (const char*)&nodelay, sizeof(nodelay));
                        }
                        // Publish under tx_mu_ so the store is ordered against
                        // the writer's snapshot of client_ (which also holds
                        // tx_mu_) — symmetric with the close side. Bump the
                        // connection epoch in the SAME lock section: this new
                        // connection gets a tag distinct from any prior one, so a
                        // frame still in flight for the previous client (even one
                        // the writer already popped) fails the writer's epoch check
                        // and is dropped rather than sent to this client.
                        {
                            std::lock_guard<std::mutex> g(tx_mu_);
                            conn_epoch_.fetch_add(1, std::memory_order_release);
                            client_.store(s, std::memory_order_release);
                        }
                        if (on_open) on_open();
                    } else {
                        CLOSESOCK(s);
                    }
                } else {
                    // We're already serving a client. Reject the 2nd
                    // connection immediately with HTTP 503 so the caller
                    // gets a clean error instead of a SYN-timeout. Drained
                    // every poll cycle so the kernel SYN queue stays empty.
                    static const char busy[] =
                        "HTTP/1.1 503 Service Unavailable\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "X-Xi-Reason: single-client-busy\r\n"
                        "\r\n";
                    ::send(s, busy, (int)sizeof(busy) - 1, 0);
                    // Graceful close so the 503 actually reaches the caller (it
                    // needs it to tell "busy" from "backend down"). closesocket()
                    // with unread inbound data RSTs, discarding the 503. So:
                    // half-close (FIN after the 503), then read until the peer
                    // closes its end (recv == 0) — bounded by a recv timeout so a
                    // misbehaving 2nd connector can't stall the poll loop.
                    // shutdown arg 1 == SD_SEND (Win32) == SHUT_WR (POSIX).
                    ::shutdown(s, 1);
                    // M4: drain NON-BLOCKING so a benign reconnect storm can't stall the
                    // poll thread (this runs on it, and it also owns the attached client's
                    // inbound + drop_requested_ + heartbeat). Was a blocking recv-loop with
                    // SO_RCVTIMEO=400ms → up to 400ms/reject on the liveness thread. One
                    // non-blocking pass reads whatever's already buffered; any unread tail
                    // is discarded by CLOSESOCK (harmless — the connection was rejected).
                    // Portable (round-3): closing with unread inbound RSTs the 503 away
                    // on POSIX too, so BOTH platforms flip the socket non-blocking
                    // (FIONBIO / O_NONBLOCK) and run the same drain loop.
#ifdef _WIN32
                    u_long nb = 1;
                    ::ioctlsocket(s, FIONBIO, &nb);
#else
                    int fl = ::fcntl(s, F_GETFL, 0);
                    if (fl != -1) ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
                    char drain[1024];
                    while (::recv(s, drain, (int)sizeof(drain), 0) > 0) { /* buffered only, no block */ }
                    CLOSESOCK(s);
                }
            }
        }

        cl = client_.load(std::memory_order_acquire);
        if (cl != INVALID_SOCK && FD_ISSET(cl, &rfds)) {
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

    // Outbound queue byte budget. If enqueuing a frame would push the total
    // buffered (unsent) outbound bytes past this, the client is a slow-but-alive
    // consumer that never trips SO_SNDTIMEO yet cannot keep up — we drop it
    // cleanly (shutdown → poll's recv returns → close_client) rather than let the
    // backend's memory grow without bound. Sized for RAW-preview streaming
    // (bench5mp): a 20MP RGB frame is ~59 MB, so the old 64 MiB cap held ONE
    // frame — any scheduling jitter meant enqueue #2 crossed the cap and dropped
    // the client on an otherwise-sustainable stream. 256 MiB ≈ a 3-4 frame burst
    // at 20MP (or ~17 frames at 5MP): plenty for jitter, still a bounded blast
    // radius for a wedged localhost client.
    static constexpr size_t kOutboundHardCapBytes = 256u * 1024u * 1024u;

    socket_t    listen_ = INVALID_SOCK;
    int         local_port_ = 0;   // actual bound port (see local_port())
    // client_ is written by the poll thread (accept / close) and read by the
    // WRITER thread inside writer_loop_ under tx_mu_. It is atomic so those reads
    // are not a data race, and — crucially — the actual CLOSESOCK is done under
    // tx_mu_ (see close_client) so the writer can never be mid-::send on an fd the
    // poll thread is closing (external review 08 finding 2). tx_mu_ is now taken
    // ONLY by the single writer thread (not every dispatch worker), so the fd-
    // reuse-UAF guard is unchanged while the blocking ::send no longer serializes
    // the workers.
    std::atomic<socket_t> client_{INVALID_SOCK};
    bool        running_ = false;
    std::vector<uint8_t> rx_buf_;
    std::mutex  tx_mu_;

    // ---- ordered async outbound writer (RT8 / doc 21 §P2) ----
    // send_frame builds the whole WS frame into one owned buffer and pushes it
    // here under out_mu_; the writer thread pops FIFO and sends. Because every
    // send_frame for the ordered lane runs UNDER the emit gate (service_inspect
    // emit_run_outcome_, between wait_turn() and complete()), enqueue order ==
    // gate order == wire order. The writer is a strict single drainer, so FIFO ==
    // exact enqueue order: never reorder or coalesce.
    //
    // Each queued frame carries the connection epoch it was built for (see
    // conn_epoch_) so a frame that outlives its connection — including one the
    // writer has already POPPED into its local variable when the swap happens — is
    // dropped instead of being sent to a newly-accepted client.
    struct OutFrame {
        uint64_t             epoch;
        std::vector<uint8_t> bytes;
    };
    std::deque<OutFrame>      out_q_;
    size_t                    out_bytes_ = 0;   // sum of out_q_ element byte sizes (guarded by out_mu_)
    std::mutex                out_mu_;
    std::condition_variable   out_cv_;
    std::thread               writer_thread_;
    bool                      writer_stop_ = false;  // guarded by out_mu_

    // ---- recycled outbound payload buffers (perf/ws-lean) ----
    // A large WS frame's buffer (a raw 5–20 MP preview is 15–60 MB) is expensive
    // to allocate: the OS commits + zero-fills fresh pages on first touch (the
    // "page-zero" fault cost), and a >1 MiB block is released straight back to the
    // OS on free, so a per-frame FRESH vector re-faults every page EVERY frame.
    // The writer returns a just-sent frame's buffer here instead of freeing it;
    // send_frame reuses one of adequate capacity, so a steady same-size stream
    // pays that fault cost ONCE, not per frame. Bounded by kBufPoolMax buffers so
    // a transient huge frame cannot pin memory (worst-case idle ≈ kBufPoolMax ×
    // max-frame). Only buffers at/above kBufPoolMinCap are pooled — tiny control-
    // frame buffers free normally. Guarded by its OWN short mutex (buf_pool_mu_),
    // taken only to pop/push a buffer — NEVER held across the big frame memcpy or
    // the ::send, so it never serializes producers or the writer.
    std::vector<std::vector<uint8_t>> buf_pool_;
    std::mutex                        buf_pool_mu_;
    static constexpr size_t kBufPoolMax    = 4;            // ≤ N idle buffers retained
    static constexpr size_t kBufPoolMinCap = 64u * 1024u; // don't pool sub-64KiB buffers

    // Take a cleared buffer with capacity ≥ need — a pooled one if available, else
    // a fresh reservation. Runs OUTSIDE out_mu_ (send_frame calls it before the
    // enqueue critical section) so the big fill/memcpy that follows is unlocked.
    std::vector<uint8_t> acquire_buf_(size_t need) {
        {
            std::lock_guard<std::mutex> g(buf_pool_mu_);
            if (!buf_pool_.empty()) {
                std::vector<uint8_t> v = std::move(buf_pool_.back());
                buf_pool_.pop_back();
                v.clear();                          // size→0, capacity (resident pages) kept
                if (v.capacity() < need) v.reserve(need);
                return v;
            }
        }
        std::vector<uint8_t> v;
        v.reserve(need);
        return v;
    }
    // Return a spent buffer for reuse (moves the storage in). Sub-64KiB buffers
    // and pool-overflow buffers are simply dropped (freed by the vector's dtor).
    void release_buf_(std::vector<uint8_t>&& v) {
        if (v.capacity() < kBufPoolMinCap) return;
        std::lock_guard<std::mutex> g(buf_pool_mu_);
        if (buf_pool_.size() < kBufPoolMax)
            buf_pool_.push_back(std::move(v));
    }

    // Connection epoch. Incremented (under tx_mu_) each time a new client is
    // published on a successful accept, so every distinct connection has a unique
    // tag. send_frame stamps the CURRENT epoch onto each queued frame; the writer,
    // under tx_mu_ (the authoritative check, same lock accept/close use), sends a
    // frame ONLY if its epoch still equals conn_epoch_. This is what actually
    // enforces "no frame crosses connections" for a frame the writer already
    // popped before a close+reaccept: tx_mu_ serializes each op atomically but does
    // NOT bind a popped frame to the fd that was live at pop time — the epoch does.
    // Atomic because send_frame reads it under out_mu_ (a DIFFERENT lock than the
    // tx_mu_ writers), so this cross-lock read must be defined/non-torn; the
    // tx_mu_-guarded store↔writer-read pair is additionally ordered by tx_mu_.
    std::atomic<uint64_t>     conn_epoch_{0};

public:
    // Test-only seam (null in production; a single null-check per frame). Invoked
    // by the writer AFTER it pops a frame and releases out_mu_, but BEFORE it
    // acquires tx_mu_ to send — i.e. inside the exact pop→send window that the
    // connection-epoch tag guards. A test sets it to force a client swap in that
    // window and prove the popped frame is dropped (epoch mismatch) instead of
    // crossing onto the new connection. Never set outside tests.
    std::function<void()> on_writer_after_pop_;

private:
    // Proactive-drop signal. The send side (send_frame's byte-cap, or the writer's
    // failed ::send) can only ::shutdown the socket — it must NOT call close_client
    // itself (close_client owns the poll-thread-only rx/msg buffers). On Windows a
    // server that shuts down its OWN socket does not make its own select() readable,
    // so a fully-wedged client that never reacts to the FIN would otherwise stay
    // attached forever (recv never returns). This flag closes that gap: the send
    // side sets it, and poll() honors it by running close_client()+on_close on the
    // poll thread within one poll tick — a bounded, peer-independent drop.
    std::atomic<bool>         drop_requested_{false};

    // Fragmented-message reassembly state (RFC 6455 §5.4). When a data
    // frame arrives with fin=0 we stash its opcode and payload; subsequent
    // continuation frames (opcode 0x0) append until fin=1.
    std::vector<uint8_t> msg_buf_;
    int                  msg_opcode_     = 0;
    bool                 msg_in_progress_ = false;

    std::string          bind_host_;     // empty/127.0.0.1 = loopback
    std::string          auth_secret_;   // empty = no auth required

    // Runs on the poll thread. The actual CLOSESOCK must not happen while a
    // worker is mid-::send on the fd (an fd-reuse UAF — external review 08
    // finding 2), yet the poll thread must not block indefinitely on tx_mu_
    // if a worker is stuck in a slow ::send. Order of operations:
    //   1. ::shutdown(fd) — unblocks any in-flight ::send (it errors out and
    //      returns) WITHOUT freeing the fd number, so no reuse can occur yet.
    //   2. Under tx_mu_: null client_ (no new send will pick up fd), then
    //      CLOSESOCK(fd). Holding tx_mu_ guarantees no worker is inside ::send
    //      on fd at the moment we close it — the in-flight one (if any) already
    //      returned in step 1 and dropped the lock.
    void close_client() {
        socket_t fd = client_.load(std::memory_order_acquire);
        if (fd == INVALID_SOCK) return;
        ::shutdown(fd, 2 /* SD_BOTH / SHUT_RDWR */);
        {
            std::lock_guard<std::mutex> g(tx_mu_);
            client_.store(INVALID_SOCK, std::memory_order_release);
            CLOSESOCK(fd);
        }
        // Drop this connection's outbound backlog so NO frame crosses connections
        // (an old client's queued frames must never reach the next client). The
        // ordering is load-bearing: we null client_ (above, under tx_mu_) BEFORE
        // clearing the queue (here, under out_mu_). A send_frame racing us either
        //   (a) acquires out_mu_ before this clear → its frame is enqueued and
        //       then removed by the clear, or
        //   (b) acquires out_mu_ after this clear → it re-reads client_ == INVALID
        //       (nulled above) and enqueues nothing.
        // Either way no QUEUED frame survives. A frame the writer has already
        // POPPED (in its local var, no longer in out_q_) is NOT covered by this
        // clear and is NOT bound to this fd by tx_mu_ (the writer fresh-loads
        // client_ after the handoff and could see a newly-accepted client). That
        // case is handled by the connection-epoch tag: the popped frame carries
        // this connection's epoch, the next accept bumps conn_epoch_, and the
        // writer's under-tx_mu_ epoch check drops the superseded frame — that
        // epoch check is the real barrier here. (M3: the notify_all below does NOT
        // make the writer "re-check client_" — the writer only ever re-evaluates
        // its wait predicate `writer_stop_ || !out_q_.empty()`, never client_; with
        // the queue just cleared the predicate is false, so a woken writer simply
        // re-parks. The notify is a harmless spurious wake, kept for stop-path
        // symmetry.)
        {
            std::lock_guard<std::mutex> g(out_mu_);
            out_q_.clear();
            out_bytes_ = 0;
        }
        // Drop the recycled-buffer pool too (perf/ws-lean): with no client attached
        // there is no stream to serve, so idle retained buffers are pure waste. The
        // next client's first few frames re-fault their pages once — negligible.
        {
            std::lock_guard<std::mutex> g(buf_pool_mu_);
            buf_pool_.clear();
        }
        // Clear any pending drop request so it can't fire on the NEXT client (the
        // one we just closed satisfied it). Safe: the send side only re-arms it
        // while a client is attached, and none is now.
        drop_requested_.store(false, std::memory_order_release);
        out_cv_.notify_all();
        // rx/msg reassembly buffers are touched only by the poll thread.
        rx_buf_.clear();
        msg_buf_.clear();
        msg_buf_.shrink_to_fit();
        msg_in_progress_ = false;
        msg_opcode_      = 0;
    }

    // The ordered outbound writer. Owns the sole blocking ::send. Runs from
    // start() to stop(). Pops one frame at a time FIFO (exact enqueue order ==
    // wire order), then sends it under tx_mu_ so the close_client fd-reuse-UAF
    // guard (external review 08 finding 2) still holds — now only THIS thread ever
    // blocks on the socket, never the dispatch workers. Snapshots client_ under
    // tx_mu_ each frame; if the connection is gone (INVALID) the frame is dropped
    // (the client it was destined for no longer exists). A short/failed ::send
    // (incl. an SO_SNDTIMEO timeout from a fully-wedged 0-drain client) means the
    // stream is desynced: ::shutdown the socket so the poll thread's recv returns
    // and close_client runs — the SAME terminal outcome as the pre-async drop, but
    // reached without ever stalling a worker.
    void writer_loop_() {
        for (;;) {
            OutFrame frame;
            {
                std::unique_lock<std::mutex> lk(out_mu_);
                out_cv_.wait(lk, [&] { return writer_stop_ || !out_q_.empty(); });
                if (writer_stop_) return;            // stop: abandon any backlog (client is going away)
                frame = std::move(out_q_.front());
                out_q_.pop_front();
                out_bytes_ -= frame.bytes.size();
            }
            // Test seam: the pop→send window the epoch tag guards (see member).
            if (on_writer_after_pop_) on_writer_after_pop_();
            std::lock_guard<std::mutex> g(tx_mu_);
            socket_t fd = client_.load(std::memory_order_acquire);
            // Authoritative cross-connection guard: send ONLY if the socket is live
            // AND this frame's epoch still matches the current connection. The
            // epoch catches the case tx_mu_ alone cannot — a frame popped for
            // client A while A is then closed and B accepted: fd would be B's
            // (valid), but frame.epoch (A's) != conn_epoch_ (B's), so we drop it.
            if (fd == INVALID_SOCK ||
                frame.epoch != conn_epoch_.load(std::memory_order_acquire)) {
                release_buf_(std::move(frame.bytes));  // recycle (perf/ws-lean)
                continue;                            // connection gone or superseded — drop
            }
            const std::vector<uint8_t>& bytes = frame.bytes;
            size_t sent = 0;
            while (sent < bytes.size()) {
                // 4 MiB chunks (matches the accepted socket's SNDBUF): big enough
                // that a 15 MB raw preview streams at memory speed, small enough
                // that SO_SNDTIMEO still bounds a wedged client per chunk (a
                // progressing client now must drain >2.7 MB/s, vs >0.7 with the
                // old 1 MiB chunks — still far below any live localhost consumer).
                int chunk = (int)std::min<size_t>(bytes.size() - sent, 4u << 20);
                int s = ::send(fd, reinterpret_cast<const char*>(bytes.data() + sent), chunk, 0);
                if (s <= 0) {
                    // Desynced/dead/wedged: half a frame went out (or SO_SNDTIMEO
                    // fired). Shut the socket AND request a proactive drop; the poll
                    // thread owns the actual close (avoids the recv/fd-reuse race)
                    // and does not depend on the peer reacting to the FIN.
                    ::shutdown(fd, 2 /* SD_BOTH / SHUT_RDWR */);
                    drop_requested_.store(true, std::memory_order_release);
                    break;
                }
                sent += (size_t)s;
            }
            release_buf_(std::move(frame.bytes));  // recycle for the next frame (perf/ws-lean)
        }
    }

    // Read HTTP request, parse Sec-WebSocket-Key, send 101 switching.
    // Any trailing bytes after the \r\n\r\n (a pipelined first frame) are
    // preserved in rx_buf_ so read_pending() can consume them on the next
    // poll cycle.
    bool do_handshake(socket_t s) {
        // P0-D5: previously do_handshake's recv() was untimed. A
        // slow-loris peer (1 byte per minute, never ending its HTTP
        // header) held the WS server's poll thread hostage. The
        // server is single-threaded so this DoS'd the entire backend.
        // Apply a per-recv timeout via SO_RCVTIMEO + cap total
        // handshake duration. Closes audit P0-D5.
        //
        // P1 (remote-mode DoS hardening): this whole read runs on the SOLE
        // poll thread BEFORE the credential check below — the auth header
        // can't be evaluated until the full HTTP header has arrived, so an
        // UNAUTHENTICATED peer that connects and then dribbles a partial
        // header (or sends nothing) pins the poll thread for the entire
        // budget. While it's pinned srv.poll() never returns, the serving
        // loop's write_heartbeat() (service_main.cpp) doesn't advance, and
        // the FE (--heartbeat-stale-ms, default 15000) force-respawns the
        // backend. The original 5 s budget let one unauth socket every ~5 s
        // starve the heartbeat chronically. The pre-auth budget therefore
        // MUST stay well under (<<) the FE heartbeat staleness threshold:
        // a real local/LAN client delivers its whole handshake header in a
        // single round-trip, so 500 ms total is generous for legitimate
        // peers while bounding the worst-case pre-auth stall (budget + one
        // in-flight recv timeout ~= 750 ms) far below the 15 s respawn
        // window. Do NOT raise these without re-checking that invariant.
        const int kHandshakeRecvTimeoutMs      = 250;  // per-recv SO_RCVTIMEO
        const int kHandshakeTotalBudgetMs      = 500;  // total pre-auth read budget; MUST be << FE heartbeat-stale-ms (15000)
        {
#ifdef _WIN32
            DWORD recv_to = (DWORD)kHandshakeRecvTimeoutMs;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&recv_to), sizeof(recv_to));
#else
            timeval recv_to{};
            recv_to.tv_sec  = kHandshakeRecvTimeoutMs / 1000;
            recv_to.tv_usec = (kHandshakeRecvTimeoutMs % 1000) * 1000;
            ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &recv_to, sizeof(recv_to));
#endif
        }
        auto t_start = std::chrono::steady_clock::now();
        auto deadline = t_start + std::chrono::milliseconds(kHandshakeTotalBudgetMs);

        std::string req;
        char buf[2048];
        size_t header_end = std::string::npos;
        for (int i = 0; i < 16; ++i) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            int n = ::recv(s, buf, (int)sizeof(buf), 0);
            if (n <= 0) return false;
            req.append(buf, buf + n);
            header_end = req.find("\r\n\r\n");
            if (header_end != std::string::npos) break;
        }
        if (header_end == std::string::npos) return false;

        auto lc = detail::to_lower(req);
        auto key_hdr = detail::get_header_(lc, req, "sec-websocket-key:");
        if (!key_hdr) return false;
        std::string key = std::move(*key_hdr);

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
            bool ok = false;
            if (auto hdr = detail::get_header_(lc, req, "authorization:")) {
                const std::string prefix = "Bearer ";
                if (hdr->size() > prefix.size() &&
                    hdr->compare(0, prefix.size(), prefix) == 0) {
                    std::string_view got(hdr->data() + prefix.size(),
                                         hdr->size() - prefix.size());
                    if (hmac_mode) {
                        // Pull X-Xi-Timestamp header.
                        if (auto ts_hdr = detail::get_header_(lc, req, "x-xi-timestamp:")) {
                            const std::string& ts_str = *ts_hdr;
                            int64_t ts = 0;
                            try { ts = std::stoll(ts_str); } catch (...) {}
                            int64_t now = (int64_t)std::time(nullptr);
                            if (ts != 0 && std::abs(now - ts) <= 60) {
                                std::string expected =
                                    xi::sha256::hmac_sha256(hmac_key, ts_str);
                                ok = detail::ct_equal(got, expected);
                            }
                        }
                    } else {
                        ok = detail::ct_equal(got, auth_secret_);
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

        // Clear the handshake-only recv timeout. Post-upgrade recv()
        // is driven by select() (read_pending) so a recv-with-timeout
        // would spuriously close idle clients; the slow-loris attack
        // surface is handshake-only.
#ifdef _WIN32
        DWORD recv_to = 0;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&recv_to), sizeof(recv_to));
#else
        timeval recv_to{};
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &recv_to, sizeof(recv_to));
#endif

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
        // Poll thread only; it is the sole closer of client_, so the fd is
        // stable across this call.
        socket_t fd = client_.load(std::memory_order_acquire);
        if (fd == INVALID_SOCK) return false;
        uint8_t tmp[16384];
        int n = ::recv(fd, reinterpret_cast<char*>(tmp), (int)sizeof(tmp), 0);
        if (n <= 0) return false;
        rx_buf_.insert(rx_buf_.end(), tmp, tmp + n);

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
        // Guard against unbounded growth: check AFTER the parse loop has
        // consumed every completed frame, so this measures the true
        // UN-parseable remainder (a partial frame's head), not a benign
        // pipelined backlog. A single recv delivering the tail of a ~max
        // frame plus the head of the next frame would momentarily exceed
        // the cap on the raw buffer; here those completed frames are
        // already erased. If what's LEFT still exceeds a legal frame's
        // worth, the peer is broken or malicious. Drop.
        if (rx_buf_.size() > kMaxFrame + 16) return false;
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

    // Build the WHOLE WS frame (header + payload) into one owned buffer and
    // ENQUEUE it for the writer thread; do NOT touch the socket. Returns true on
    // ENQUEUE (see the file-header note — WS send was never a delivery guarantee),
    // false only when there is no client OR the hard byte-cap fired and the wedged
    // client was dropped. Callable from any thread; when called under the ordered-
    // emit gate the enqueue order is the wire order.
    //
    // Cost: one owned buffer holding a copy of the caller's payload (up to ~16 MiB
    // for a max preview). That extra memcpy is unavoidable — the caller's buffer
    // is transient (a staged sink's scratch) and must not outlive this call — and
    // is a bounded, memcpy-scale price versus the pre-async cost: a blocking
    // ::send that could stall the whole ordered lane on the client's socket for up
    // to SO_SNDTIMEO (1.5 s) per frame.
    bool send_frame(int opcode, const uint8_t* data, size_t n) {
        // Header (2/4/10 bytes) then payload, all into one buffer.
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
        std::vector<uint8_t> frame = acquire_buf_(hlen + n);  // pooled buffer (perf/ws-lean)
        frame.insert(frame.end(), hdr, hdr + hlen);
        if (n) frame.insert(frame.end(), data, data + n);

        std::lock_guard<std::mutex> g(out_mu_);
        // Re-check client_ UNDER out_mu_ (paired with close_client's null-then-
        // clear ordering): no client → nothing to send. This also means a frame
        // built for a just-departed client is never enqueued.
        if (client_.load(std::memory_order_acquire) == INVALID_SOCK) return false;
        // Backpressure: a slow-but-alive consumer that never trips SO_SNDTIMEO can
        // still let the backlog grow without bound. When it would cross the cap,
        // wedge-drop the client cleanly (shutdown → poll's recv returns →
        // close_client), clear the backlog, and fail this send. We drop the WHOLE
        // client rather than silently discarding individual frames: a dropped frame
        // desyncs the WS stream and gives a monitor a phantom gap; a clean full drop
        // is honest and terminal (same outcome the FE already handles for a
        // SO_SNDTIMEO drop).
        if (out_bytes_ + frame.size() > kOutboundHardCapBytes) {
            socket_t fd = client_.load(std::memory_order_acquire);
            if (fd != INVALID_SOCK) {
                ::shutdown(fd, 2 /* SD_BOTH / SHUT_RDWR */);
                drop_requested_.store(true, std::memory_order_release);  // poll() finishes the close
            }
            out_q_.clear();
            out_bytes_ = 0;
            return false;
        }
        // Stamp the frame with the CURRENT connection epoch (read here under
        // out_mu_; the authoritative match happens in the writer under tx_mu_).
        uint64_t ep = conn_epoch_.load(std::memory_order_acquire);
        out_bytes_ += frame.size();
        out_q_.push_back(OutFrame{ ep, std::move(frame) });
        out_cv_.notify_one();
        return true;
    }
};

} // namespace xi::ws
