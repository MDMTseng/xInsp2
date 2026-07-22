//
// test_ws_exclusive_bind.cpp — proves the two properties of the exclusive-bind
// hardening (polaris2/exclusive-bind), spun out of the ws_teardown_race
// forensics.
//
// Background: the production WS listen socket used SO_REUSEADDR. On Windows
// SO_REUSEADDR does NOT mean "reuse a TIME_WAIT port" the way it does on POSIX —
// it permits a FULL DUPLICATE ACTIVE BIND: any other local process can bind the
// backend's exact ip:port while the backend is listening, and the kernel then
// splits inbound connections between the two rival listeners nondeterministically
// (connection hijack; observed live during the deflake, two listeners on port
// 39187 each getting a share of connects). The fix swaps SO_REUSEADDR for
// SO_EXCLUSIVEADDRUSE on the Windows listen socket.
//
// Two properties, two tests:
//   (a) HIJACK PREVENTION — with a server listening on a port, a rival socket
//       (setting SO_REUSEADDR, the actual attacker vector) that tries to bind the
//       SAME port must now FAIL. Under the old SO_REUSEADDR listener it would
//       have succeeded — that was the hijack.
//   (b) RAPID RESPAWN — the FE respawns the backend by closing the old listener
//       and having the new backend bind the SAME fixed port immediately. This is
//       the behaviour SO_REUSEADDR was presumably protecting. SO_EXCLUSIVEADDRUSE
//       must NOT reintroduce Linux-style TIME_WAIT rebind pain on a listen
//       socket: a fast start→stop→start cycle on one fixed port must still
//       succeed every time. If it does NOT, this test fails loudly — do not ship
//       a change that breaks FE respawn.
//
// Windows-only, matching xi_ws_server.hpp's platform.
//

#include <xi/xi_ws_server.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#define CHECK(expr)                                                  \
    do {                                                             \
        if (!(expr)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n",                 \
                __FILE__, __LINE__, #expr);                          \
            std::abort();                                            \
        }                                                            \
    } while (0)

static int stress_scale() {
    static int s = [] {
        const char* e = std::getenv("XINSP2_STRESS_SCALE");
        int v = e ? std::atoi(e) : 1;
        return v > 0 ? v : 1;
    }();
    return s;
}

#ifdef _WIN32

// Minimal WS client: TCP connect + a valid RFC 6455 handshake so the server
// promotes us to a live client_ (fires on_open). Mirrors the helper in
// test_ws_teardown_race.cpp.
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
    if (::send(s, req, (int)std::strlen(req), 0) <= 0) {
        CLOSESOCK(s);
        return INVALID_SOCK;
    }
    std::string resp;
    char buf[1024];
    for (int i = 0; i < 16; ++i) {
        int n = ::recv(s, buf, (int)sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, buf + n);
        if (resp.find("\r\n\r\n") != std::string::npos) break;
    }
    if (resp.find(" 101 ") == std::string::npos) {
        CLOSESOCK(s);
        return INVALID_SOCK;
    }
    return s;
}

// Attempt a raw ::bind on loopback:port with SO_REUSEADDR set — exactly what a
// rival process would do to try to duplicate-bind (hijack) the listener's port.
// Returns true if the bind SUCCEEDED (i.e. the port was hijackable). Cleans up
// its own socket.
static bool rival_bind_succeeds(int port) {
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCK) return false;
    int opt = 1;
    // SO_REUSEADDR is the attacker's lever: on Windows it is what USED to make a
    // duplicate active bind possible against a SO_REUSEADDR listener.
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons((u_short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool ok = (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    CLOSESOCK(s);
    return ok;
}

// ---- Property (a): hijack prevention -------------------------------------
static void test_hijack_prevention() {
    std::fprintf(stderr, "--- hijack prevention ---\n");
    xi::ws::Server srv;
    // Ephemeral port: unique to this process's listen socket for its lifetime,
    // so the test never collides with a concurrent run — yet the rival-bind
    // attempt below targets that exact same port.
    CHECK(srv.start(0));
    const int port = srv.local_port();
    CHECK(port > 0);

    // A rival tries to duplicate-bind the live listener's port. With the old
    // SO_REUSEADDR listener this SUCCEEDED (the hijack). With SO_EXCLUSIVEADDRUSE
    // it must FAIL.
    bool hijacked = rival_bind_succeeds(port);
    std::fprintf(stderr, "  port=%d rival_bind_succeeded=%d (want 0)\n",
                 port, (int)hijacked);
    CHECK(!hijacked);

    srv.stop();

    // Sanity: once the listener is gone the port is free again, so a bind now
    // SUCCEEDS — proves the failure above was the live exclusive listener, not a
    // permanently-wedged port.
    bool free_after = rival_bind_succeeds(port);
    std::fprintf(stderr, "  after stop, bind_succeeded=%d (want 1)\n",
                 (int)free_after);
    CHECK(free_after);
    std::fprintf(stderr, "  hijack prevention: OK\n");
}

// ---- Property (b): rapid respawn on a fixed port -------------------------
static void test_rapid_respawn() {
    std::fprintf(stderr, "--- rapid respawn (fixed port) ---\n");
    // Discover a currently-free port via an ephemeral bind, then release it and
    // reuse that SAME fixed port for every respawn cycle — modelling the FE
    // closing the old BE listener and the new BE binding the same configured
    // port. SO_EXCLUSIVEADDRUSE must not make any cycle's bind fail.
    int port = 0;
    {
        xi::ws::Server probe;
        CHECK(probe.start(0));
        port = probe.local_port();
        CHECK(port > 0);
        probe.stop();
    }
    std::fprintf(stderr, "  respawn port=%d\n", port);

    const int CYCLES = 50 * stress_scale();
    for (int i = 0; i < CYCLES; ++i) {
        xi::ws::Server srv;
        std::atomic<long> opens{0};
        srv.on_open = [&] { opens.fetch_add(1, std::memory_order_relaxed); };

        // The property under test: rebinding the same fixed port right after the
        // previous listener closed must SUCCEED. A failure here is exactly the FE
        // respawn breakage we must not ship — report loudly.
        if (!srv.start(port)) {
            std::fprintf(stderr,
                "FAIL respawn cycle %d/%d: start(%d) FAILED — "
                "SO_EXCLUSIVEADDRUSE broke rapid rebind (FE respawn regression)\n",
                i, CYCLES, port);
            std::abort();
        }
        CHECK(srv.local_port() == port);

        // Drive one real connect/accept/teardown so each cycle exercises the full
        // listen→serve→close path the FE respawn actually goes through, not just
        // bind/unbind.
        std::atomic<bool> stop{false};
        std::thread poll_thread([&] {
            while (!stop.load(std::memory_order_relaxed)) srv.poll(1);
        });
        socket_t c = connect_and_handshake(port);
        if (c != INVALID_SOCK) {
            // Wait briefly for the poll thread to accept + fire on_open.
            for (int w = 0; w < 200 && opens.load() == 0; ++w)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CLOSESOCK(c);
        }
        stop.store(true, std::memory_order_relaxed);
        poll_thread.join();
        srv.stop();
    }
    std::fprintf(stderr, "  rapid respawn: OK (%d cycles)\n", CYCLES);
}

int main() {
    std::fprintf(stderr, "=== test_ws_exclusive_bind ===\n");
    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    test_hijack_prevention();
    test_rapid_respawn();

    std::fprintf(stderr, "\nALL TESTS PASSED\n");
    return 0;
}

#else  // !_WIN32

int main() {
    std::fprintf(stderr, "test_ws_exclusive_bind: skipped (Windows-only)\n");
    return 0;
}

#endif
