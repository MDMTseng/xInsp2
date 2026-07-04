//
// test_ws_async_writer.cpp — proves the RT8 / doc 21 §P2 fix: the outbound
// WS send is decoupled from the client's socket drain rate by the Server's
// ordered async writer thread (see xi_ws_server.hpp / docs/new_gen/23-rt8-
// async-writer.md).
//
// The bug: send_frame held tx_mu_ across a blocking ::send, called from dispatch
// workers INSIDE the ordered-emit gate. A slow/wedged-but-alive client pinned the
// whole ordered lane on its socket drain rate (up to SO_SNDTIMEO = 1.5 s/frame).
//
// This is the deterministic, send-scale-vs-memcpy-scale unit proof (the qa_slow_
// consumer example proves lane liveness end-to-end). Four assertions:
//
//   A. DECOUPLING — with a fully-wedged client (never drains), EVERY send_binary
//      CALL returns in memcpy time (< kMaxCallMs), never the ~1.5 s a blocking
//      ::send on a full socket would take. This is THE gate-critical-section
//      bound.
//   B. LIVENESS   — in a fixed 1 s window against that wedged client, the caller
//      completes thousands of send_binary calls (pre-fix it would complete ~1
//      then block for 1.5 s). The lane keeps moving.
//   C. BACKPRESSURE + CLEAN DROP — the wedged client is dropped within a bounded
//      time (on_close fires, has_client()==false); at least one send returns
//      false (the byte-cap or SO_SNDTIMEO drop engaged) rather than buffering
//      without bound.
//   D. ORDER — a well-behaved draining client receives frames in strict FIFO
//      (single-drainer == exact enqueue order == wire order).
//
// Windows-only, matching xi_ws_server.hpp's platform.
//

#include <xi/xi_ws_server.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                __FILE__, __LINE__, #expr);                          \
            std::abort();                                            \
        }                                                            \
    } while (0)

#ifdef _WIN32

// TCP connect + a valid RFC 6455 handshake so the server promotes us to a live
// client_ (fires on_open). Returns the connected socket, or INVALID_SOCK.
static socket_t connect_and_handshake(int port) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCK) return INVALID_SOCK;
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        CLOSESOCK(s);
        return INVALID_SOCK;
    }
    const char* req =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";
    if (::send(s, req, (int)std::strlen(req), 0) <= 0) { CLOSESOCK(s); return INVALID_SOCK; }
    std::string resp;
    char buf[1024];
    for (int i = 0; i < 16; ++i) {
        int n = ::recv(s, buf, (int)sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, buf + n);
        if (resp.find("\r\n\r\n") != std::string::npos) break;
    }
    if (resp.find(" 101 ") == std::string::npos) { CLOSESOCK(s); return INVALID_SOCK; }
    return s;
}

// Read exactly n bytes into out (blocking). false on peer close/error.
static bool recv_exact(socket_t s, uint8_t* out, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = ::recv(s, reinterpret_cast<char*>(out + got), (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// Read one server->client WS frame (server frames are unmasked, always fin). Puts
// the payload into `payload`. Returns false on peer close/error.
static bool read_ws_frame(socket_t s, std::vector<uint8_t>& payload) {
    uint8_t h2[2];
    if (!recv_exact(s, h2, 2)) return false;
    uint64_t len = h2[1] & 0x7F;             // server never sets the mask bit
    if (len == 126) {
        uint8_t e[2];
        if (!recv_exact(s, e, 2)) return false;
        len = ((uint64_t)e[0] << 8) | e[1];
    } else if (len == 127) {
        uint8_t e[8];
        if (!recv_exact(s, e, 8)) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
    }
    payload.resize((size_t)len);
    if (len && !recv_exact(s, payload.data(), (size_t)len)) return false;
    return true;
}

static void wait_until(std::atomic<bool>& flag, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int main() {
    std::fprintf(stderr, "=== test_ws_async_writer ===\n");

    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    xi::ws::Server srv;
    std::atomic<bool> opened{false};
    std::atomic<int>  closes{0};
    srv.on_open  = [&] { opened.store(true, std::memory_order_release); };
    srv.on_close = [&] { closes.fetch_add(1, std::memory_order_relaxed); };

    CHECK(srv.start(0));   // ephemeral port — unique to this process (see teardown-race note)
    const int port = srv.local_port();
    CHECK(port > 0);

    std::atomic<bool> stop_poll{false};
    std::thread poll_thread([&] {
        while (!stop_poll.load(std::memory_order_relaxed)) srv.poll(1);
    });

    // ------------------------------------------------------------------
    // Phase 1: WEDGED client. Handshake, then NEVER recv. Its TCP receive
    // window fills; the writer thread's ::send blocks, and the caller (us,
    // standing in for a dispatch worker in the emit gate) must NOT.
    // ------------------------------------------------------------------
    socket_t wedged = connect_and_handshake(port);
    CHECK(wedged != INVALID_SOCK);
    wait_until(opened, 4000);
    CHECK(opened.load());
    CHECK(srv.has_client());

    const size_t kFrame   = 64 * 1024;     // 64 KiB payloads
    const double kMaxCallMs = 200.0;       // memcpy-scale bound (real ~<5ms); NOT the 1.5s send
    std::vector<uint8_t> payload(kFrame, 0xAB);

    double max_call_ms = 0.0;
    long   calls_in_1s = 0;
    bool   saw_false   = false;
    long   total_calls = 0;

    auto t0 = std::chrono::steady_clock::now();
    // Run up to 4 s, or until the wedged client is dropped, whichever first.
    while (true) {
        auto now = std::chrono::steady_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(now - t0).count();
        if (elapsed_ms > 4000.0) break;
        if (closes.load(std::memory_order_relaxed) > 0) break;

        auto c0 = std::chrono::steady_clock::now();
        bool ok = srv.send_binary(payload.data(), payload.size());
        auto c1 = std::chrono::steady_clock::now();
        double call_ms = std::chrono::duration<double, std::milli>(c1 - c0).count();

        if (call_ms > max_call_ms) max_call_ms = call_ms;
        if (!ok) saw_false = true;
        ++total_calls;
        if (elapsed_ms <= 1000.0) ++calls_in_1s;
    }

    // Give the wedged client's terminal drop time to complete (SO_SNDTIMEO ~1.5s
    // on the writer, then poll's recv returns -> close_client).
    std::atomic<bool> dropped{false};
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(6000);
        while (closes.load(std::memory_order_relaxed) == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        dropped.store(closes.load(std::memory_order_relaxed) > 0);
    }
    CLOSESOCK(wedged);

    std::fprintf(stderr,
        "  phase1(wedged): total_calls=%ld calls_in_1s=%ld max_call_ms=%.2f "
        "saw_false=%d closes=%d\n",
        total_calls, calls_in_1s, max_call_ms, (int)saw_false, closes.load());

    // A. DECOUPLING: no single enqueue call anywhere near a blocking send.
    CHECK(max_call_ms < kMaxCallMs);
    // B. LIVENESS: the caller kept moving (pre-fix ~1 call, then a 1.5s block).
    CHECK(calls_in_1s > 200);
    // C. BACKPRESSURE + CLEAN DROP: bounded buffering engaged, client dropped.
    CHECK(saw_false);
    CHECK(dropped.load());
    // has_client() must be false again once the drop completed.
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (srv.has_client() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(!srv.has_client());

    // ------------------------------------------------------------------
    // Phase 2: ORDER. A well-behaved client that drains fully must receive
    // every frame in strict enqueue order (FIFO single drainer).
    // ------------------------------------------------------------------
    opened.store(false);
    socket_t fastc = connect_and_handshake(port);
    CHECK(fastc != INVALID_SOCK);
    wait_until(opened, 4000);
    CHECK(opened.load());

    const int kN = 300;
    std::atomic<bool> order_ok{true};
    std::atomic<int>  received{0};
    std::thread reader([&] {
        std::vector<uint8_t> pl;
        for (int i = 0; i < kN; ++i) {
            if (!read_ws_frame(fastc, pl)) break;
            if (pl.size() < 4) { order_ok.store(false); break; }
            uint32_t seq = ((uint32_t)pl[0] << 24) | ((uint32_t)pl[1] << 16) |
                           ((uint32_t)pl[2] << 8) | (uint32_t)pl[3];
            if (seq != (uint32_t)i) { order_ok.store(false); break; }
            received.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<uint8_t> obuf(8 * 1024, 0);
    for (int i = 0; i < kN; ++i) {
        obuf[0] = (uint8_t)((i >> 24) & 0xFF);
        obuf[1] = (uint8_t)((i >> 16) & 0xFF);
        obuf[2] = (uint8_t)((i >> 8) & 0xFF);
        obuf[3] = (uint8_t)(i & 0xFF);
        CHECK(srv.send_binary(obuf.data(), obuf.size()));
    }
    reader.join();

    std::fprintf(stderr, "  phase2(order): received=%d/%d order_ok=%d\n",
        received.load(), kN, (int)order_ok.load());
    CHECK(order_ok.load());
    CHECK(received.load() == kN);
    CLOSESOCK(fastc);

    stop_poll.store(true);
    poll_thread.join();
    srv.stop();
    srv.stop();   // idempotent double-stop (also exercised by ~Server)

    std::fprintf(stderr, "\nALL TESTS PASSED\n");
    return 0;
}

#else  // !_WIN32

int main() {
    std::fprintf(stderr, "test_ws_async_writer: skipped (Windows-only)\n");
    return 0;
}

#endif
