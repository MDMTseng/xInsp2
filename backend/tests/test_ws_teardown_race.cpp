//
// test_ws_teardown_race.cpp — provokes external review 08 finding 2: the
// WebSocket client_ socket was closed/reset by the poll thread OUTSIDE tx_mu_
// while dispatch workers sent frames UNDER tx_mu_. A disconnect racing a send
// is a data race on client_ and a use-after-close / fd-reuse window: a worker
// could ::send WebSocket bytes into a freshly-recycled fd.
//
// This test drives that exact race: N sender threads hammer srv.send_text()
// (the worker side) while a client thread repeatedly connects and abruptly
// disconnects, forcing the poll thread through close_client() over and over,
// concurrent with the in-flight sends. Before the fix (close off-lock) this
// is UB and can fault / ASAN-trip; after the fix (::shutdown then CLOSESOCK
// under tx_mu_) every close is serialized against every send, so it runs
// clean. Success == the whole churn completes without a crash.
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
#include <vector>

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
// promotes us to a live client_ (fires on_open). We never send data frames —
// we only need client_ to become valid, then to drop the connection so the
// poll thread runs close_client() while senders are mid-send.
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
    // "dGhlIHNhbXBsZSBub25jZQ==" is the RFC 6455 example key; any base64 works,
    // the server just echoes the derived Accept — we don't verify it.
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

int main() {
    std::fprintf(stderr, "=== test_ws_teardown_race ===\n");

    WSADATA wsd;
    WSAStartup(MAKEWORD(2, 2), &wsd);

    xi::ws::Server srv;
    std::atomic<long> opens{0};
    std::atomic<long> closes{0};
    srv.on_open  = [&] { opens.fetch_add(1, std::memory_order_relaxed); };
    srv.on_close = [&] { closes.fetch_add(1, std::memory_order_relaxed); };

    // Loopback, ephemeral-ish fixed port. If it's busy the bind fails; pick a
    // high port unlikely to collide with the running backend (7823).
    const int port = 39187;
    CHECK(srv.start(port));

    std::atomic<bool> stop{false};

    // Poll thread — the sole owner of accept() and close_client().
    std::thread poll_thread([&] {
        while (!stop.load(std::memory_order_relaxed)) srv.poll(1);
    });

    // Sender threads — the dispatch-worker side. Hammer send_text regardless of
    // whether a client is currently attached (send_frame returns false with no
    // client; that's fine — we want the calls in flight when a close lands).
    constexpr int SENDERS = 4;
    std::atomic<long> sends{0};
    std::vector<std::thread> senders;
    // A payload large enough that a single send spans multiple ::send chunks /
    // takes long enough to overlap a concurrent close, but under kMaxFrame.
    std::string payload(64 * 1024, 'x');
    for (int t = 0; t < SENDERS; ++t) {
        senders.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                srv.send_text(payload);
                sends.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Client churn: connect, let a few sends land, disconnect abruptly. Each
    // abrupt close forces the poll thread through close_client() concurrently
    // with the senders — the race under test.
    const int ROUNDS = 300 * stress_scale();
    long connected = 0;
    for (int r = 0; r < ROUNDS; ++r) {
        socket_t c = connect_and_handshake(port);
        if (c == INVALID_SOCK) {
            // Server may still be finishing a previous close; retry shortly.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        ++connected;
        // Let the connection live briefly so senders push real frames into it.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // Abrupt close (no WS close frame) — the poll thread's read_pending
        // fails and close_client() runs, racing the in-flight sends.
        CLOSESOCK(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& s : senders) s.join();
    poll_thread.join();
    srv.stop();

    std::fprintf(stderr,
        "  connected=%ld opens=%ld closes=%ld sends=%ld\n",
        connected, opens.load(), closes.load(), sends.load());
    // We provoked real connects (so the race window actually opened) and did
    // not crash — that is the test.
    CHECK(connected > 0);
    CHECK(opens.load() > 0);

    std::fprintf(stderr, "\nALL TESTS PASSED\n");
    return 0;
}

#else  // !_WIN32

int main() {
    std::fprintf(stderr, "test_ws_teardown_race: skipped (Windows-only)\n");
    return 0;
}

#endif
