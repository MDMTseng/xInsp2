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

// Ownership-token release for the zero-copy owned-send test (Phase 4): a plain
// fn pointer (non-capturing) the Server calls exactly once per frame. Bumps the
// int the token points at, so the test can assert release happened once — after
// a send AND on a no-client early return, never twice (no double free).
static void owned_release_counter(void* p) {
    reinterpret_cast<std::atomic<int>*>(p)->fetch_add(1, std::memory_order_relaxed);
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

    // ------------------------------------------------------------------
    // Phase 3: CONNECTION-EPOCH GUARD. A frame the writer has already POPPED
    // (for client A) must NOT be sent to a newly-accepted client B if A is
    // closed and B accepted in the pop→send window. tx_mu_ alone does not bind
    // the popped frame to A's fd (the writer fresh-loads client_ after the
    // handoff); the connection-epoch tag is what drops it. We force exactly that
    // interleave deterministically via the on_writer_after_pop_ test seam.
    // ------------------------------------------------------------------
    {
        // Wait for the phase-2 client's close to settle.
        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (srv.has_client() && std::chrono::steady_clock::now() < dl)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        std::atomic<bool> in_pop_window{false};
        std::atomic<bool> resume_writer{false};
        std::atomic<int>  hook_calls{0};
        srv.on_writer_after_pop_ = [&] {
            if (hook_calls.fetch_add(1) != 0) return;   // one-shot: only the F frame
            in_pop_window.store(true, std::memory_order_release);
            while (!resume_writer.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };

        // Client A.
        opened.store(false);
        socket_t A = connect_and_handshake(port);
        CHECK(A != INVALID_SOCK);
        wait_until(opened, 4000);
        CHECK(opened.load());

        // Enqueue frame F for A (marker byte 0xF1). The writer pops it, then parks
        // in the hook inside the pop→send window.
        std::vector<uint8_t> F(2048, 0xF1);
        CHECK(srv.send_binary(F.data(), F.size()));
        {
            auto d2 = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
            while (!in_pop_window.load() && std::chrono::steady_clock::now() < d2)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(in_pop_window.load());   // writer holds F, parked before tx_mu_

        // Swap A -> B while the writer is parked. Close A (poll runs close_client,
        // client_ -> INVALID), then accept B (poll bumps conn_epoch_, publishes B).
        CLOSESOCK(A);
        {
            auto d3 = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
            while (srv.has_client() && std::chrono::steady_clock::now() < d3)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(!srv.has_client());
        opened.store(false);
        socket_t B = connect_and_handshake(port);
        CHECK(B != INVALID_SOCK);
        wait_until(opened, 4000);
        CHECK(opened.load());
        // Bound B's reader so it can't block forever on the (correct) absence of F.
        DWORD rto = 800;
        ::setsockopt(B, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rto, sizeof(rto));

        // B's reader: record the marker of every frame it receives.
        std::atomic<bool> saw_F{false};
        std::atomic<bool> saw_G{false};
        std::thread b_reader([&] {
            std::vector<uint8_t> pl;
            while (read_ws_frame(B, pl)) {
                if (!pl.empty() && pl[0] == 0xF1) saw_F.store(true);
                if (!pl.empty() && pl[0] == 0x60) { saw_G.store(true); break; }
            }
        });

        // Release the writer: it now sees client_ == B (valid fd) but F.epoch !=
        // conn_epoch_ (B's), so it MUST drop F rather than send it to B.
        resume_writer.store(true, std::memory_order_release);

        // Then enqueue a fresh frame G (marker 0x60) for B — its epoch matches, so
        // B must receive it. This confirms the guard drops only the superseded
        // frame, not a legitimate same-connection one.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::vector<uint8_t> G(2048, 0x60);
        CHECK(srv.send_binary(G.data(), G.size()));
        b_reader.join();

        srv.on_writer_after_pop_ = nullptr;
        std::fprintf(stderr, "  phase3(epoch): saw_F(crossed)=%d saw_G(same-conn)=%d\n",
            (int)saw_F.load(), (int)saw_G.load());
        CHECK(!saw_F.load());   // the popped A-frame did NOT cross onto B
        CHECK(saw_G.load());    // a legitimate B-frame is still delivered
        CLOSESOCK(B);
    }

    // ------------------------------------------------------------------
    // Phase 4: ZERO-COPY OWNED PATH (perf/ws-lean, xi.emit@2 / send_binary_owned).
    // Proves: (E1) an owned send assembled from MULTIPLE borrowed segments puts
    // BYTE-IDENTICAL wire bytes on the socket as the copy path (send_binary) does
    // for the same concatenated payload, and its ownership token is released
    // EXACTLY ONCE after the send; (E2) an owned send with NO client returns false
    // and STILL releases the token exactly once (no leak) — never twice (no double
    // free). The RAII owner-release in OutFrame is what makes every drop path free
    // the producer's bytes.
    // ------------------------------------------------------------------
    {
        opened.store(false);
        socket_t c = connect_and_handshake(port);
        CHECK(c != INVALID_SOCK);
        wait_until(opened, 4000);
        CHECK(opened.load());

        // A payload split across two segments; the wire frame must be their exact
        // concatenation, and must equal what the copy path sends for the whole.
        std::vector<uint8_t> seg0 = { 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03 };
        std::vector<uint8_t> seg1(5000);
        for (size_t i = 0; i < seg1.size(); ++i) seg1[i] = (uint8_t)(i * 31u + 7u);
        std::vector<uint8_t> whole;
        whole.insert(whole.end(), seg0.begin(), seg0.end());
        whole.insert(whole.end(), seg1.begin(), seg1.end());

        // Reader pulls exactly two frames: [0] copy path, [1] owned path.
        std::vector<std::vector<uint8_t>> got(2);
        std::atomic<bool> read_ok{true};
        std::thread rdr([&] {
            for (int i = 0; i < 2; ++i)
                if (!read_ws_frame(c, got[(size_t)i])) { read_ok.store(false); break; }
        });

        // Frame 0: copy path (reference bytes).
        CHECK(srv.send_binary(whole.data(), whole.size()));

        // Frame 1: owned path, two segments, with a release-counting token.
        std::atomic<int> rel{0};
        xi::ws::BinSpan segs[2] = {
            { seg0.data(), seg0.size() },
            { seg1.data(), seg1.size() },
        };
        CHECK(srv.send_binary_owned(segs, 2, &rel, &owned_release_counter));

        rdr.join();
        CHECK(read_ok.load());
        // E1: byte-identical payloads, and both equal the intended whole.
        CHECK(got[0] == whole);
        CHECK(got[1] == whole);
        CHECK(got[0] == got[1]);
        // E1: the owned token was released exactly once (after the send).
        {
            auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            while (rel.load() < 1 && std::chrono::steady_clock::now() < dl)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(rel.load() == 1);

        // E2: with no client attached, an owned send returns false but STILL
        // releases the token exactly once (no leak, no double free).
        CLOSESOCK(c);
        {
            auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            while (srv.has_client() && std::chrono::steady_clock::now() < dl)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(!srv.has_client());
        std::atomic<int> rel2{0};
        xi::ws::BinSpan one = { whole.data(), whole.size() };
        CHECK(!srv.send_binary_owned(&one, 1, &rel2, &owned_release_counter));
        CHECK(rel2.load() == 1);

        std::fprintf(stderr, "  phase4(owned): rel=%d rel_noclient=%d bytes_identical=%d\n",
            rel.load(), rel2.load(), (int)(got[0] == got[1]));
    }

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
