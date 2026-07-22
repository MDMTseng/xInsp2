//
// bench_ws_blast.cpp — raw-frame WebSocket egress ceiling probe (experiment
// exp/ws-zerocopy-send, 2026-07).
//
// QUESTION (CT): "完全無 memcpy 的 websocket send，raw 5MP / 20MP 最大 fps 是多少?"
//
// The zero-copy send path ALREADY exists: xi.ws::Server::send_binary_owned
// (xi.emit@2 / perf/ws-lean, doc 32) streams the payload straight from the
// producer's borrowed bytes — the host copies NO payload, only the ~10-byte WS
// header. This bench measures the CEILING that path can hit for a raw frame, and
// contrasts it with the copying send_binary path, on THIS box, with a tight
// in-process raw drain (no producer chain, no encode — a pure writer/socket
// probe). It reproduces the doc-32 "blast" probe whose harness was scratch and
// never landed, and directly answers CT's fps question.
//
// WHAT IT DOES
//   - Spins up a real xi::ws::Server on an ephemeral loopback port, driven by a
//     poll thread (accept + writer).
//   - Connects an in-process Winsock client that does the RFC-6455 handshake and
//     then drains as fast as recv() allows, counting raw bytes (NO WS-frame
//     parse — the fastest possible consumer, same as doc-32's raw drain).
//   - For each (resolution, path) it blasts ONE fixed hot frame for a fixed
//     window, pacing by retry-on-false (never drops a frame: a false return means
//     the 256 MiB byte-cap is momentarily full, so we yield and re-offer the SAME
//     frame). Throughput is measured at the CLIENT (bytes actually delivered),
//     which is ground truth for the delivered fps.
//
// OUTPUT: for 5 MP / 20 MP, mono8 + RGB, both send paths: sustained MB/s and fps.
//
// This is a manual experiment target — built, NOT wired into ctest.
//
#include "xi/xi_ws_server.hpp"   // brings in Winsock2 + Ws2_32 (pragma comment lib)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// In-process WS client: connect to 127.0.0.1:port, do the handshake, then drain
// raw bytes into an atomic counter until told to stop. No frame parse — we only
// need the delivered BYTE rate, so we count everything recv() hands back.
// ---------------------------------------------------------------------------
struct DrainClient {
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool>     stop{false};
    std::atomic<bool>     connected{false};
    std::thread           th;
    socket_t              sock = INVALID_SOCK;

    void start(int port) {
        th = std::thread([this, port] { run(port); });
    }
    void join() { stop.store(true); if (th.joinable()) th.join(); }

    void run(int port) {
        sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCK) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((u_short)port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            CLOSESOCK(sock); sock = INVALID_SOCK; return;
        }
        // Grow the client RECV buffer so a slow drain isn't the bottleneck (mirror
        // of the server's adaptive 4 MiB SNDBUF — we want to probe the writer/socket
        // wall, not a starved receiver).
        int rcv = 4 * 1024 * 1024;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcv), sizeof(rcv));

        // RFC-6455 handshake. Any valid 24-char base64 nonce works — the server
        // only echoes the derived accept key; we don't verify it here.
        const char* req =
            "GET / HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        if (::send(sock, req, (int)std::strlen(req), 0) <= 0) { CLOSESOCK(sock); sock = INVALID_SOCK; return; }

        // Read until the end of the 101 response header (\r\n\r\n). Anything past it
        // is already frame bytes — count it.
        std::string hs;
        char buf[8192];
        for (;;) {
            int n = ::recv(sock, buf, (int)sizeof(buf), 0);
            if (n <= 0) { CLOSESOCK(sock); sock = INVALID_SOCK; return; }
            hs.append(buf, buf + n);
            size_t hdr_end = hs.find("\r\n\r\n");
            if (hdr_end != std::string::npos) {
                size_t body = hdr_end + 4;
                if (body < hs.size()) bytes.fetch_add(hs.size() - body, std::memory_order_relaxed);
                break;
            }
            if (hs.size() > 64 * 1024) { CLOSESOCK(sock); sock = INVALID_SOCK; return; }  // runaway
        }
        connected.store(true, std::memory_order_release);

        // Tight drain loop: recv the fastest way we can and just tally bytes.
        std::vector<char> big(4 * 1024 * 1024);
        while (!stop.load(std::memory_order_acquire)) {
            int n = ::recv(sock, big.data(), (int)big.size(), 0);
            if (n <= 0) break;
            bytes.fetch_add((uint64_t)n, std::memory_order_relaxed);
        }
        if (sock != INVALID_SOCK) { CLOSESOCK(sock); sock = INVALID_SOCK; }
    }
};

// no-op owner release for the owned path: we reuse ONE persistent frame buffer for
// every send, so nothing is actually freed — the token just satisfies the API's
// non-null owner+release contract.
static void noop_release(void*) {}

struct Scenario {
    const char* name;
    size_t      frame_bytes;
};

// Total payload bytes we have OFFERED across the whole bench (single producer =
// main thread, so a plain counter is fine). Paced against cli.bytes (delivered)
// for backpressure — see blast().
static uint64_t g_offered = 0;

// The server's byte-cap (kOutboundHardCapBytes) DROPS the client when the unsent
// backlog crosses it — it is slow-consumer protection, NOT retryable backpressure.
// So we must keep the in-flight backlog (offered − delivered-at-client) BELOW the
// cap ourselves. Guard = min(8×frame, cap/2) — doc-32's pacing-guard fix: deep
// enough to keep the writer/socket saturated (never starved), shallow enough that
// it can't trip the drop. cap/2 = 128 MiB.
static constexpr uint64_t kServerCap = 256ull * 1024 * 1024;

// Blast one fixed `frame` through the server for `window_s` seconds via the chosen
// path, measuring delivered bytes at the client. Returns {MB/s, fps}; fps < 0
// signals the client was dropped (unexpected — pacing should prevent it).
struct Measure { double mbps; double fps; };

static Measure blast(xi::ws::Server& srv, DrainClient& cli,
                     const std::vector<std::vector<uint8_t>>& bufs, bool owned,
                     double warmup_s, double window_s) {
    const size_t frame_bytes = bufs[0].size();
    const uint64_t guard = std::min<uint64_t>(8ull * frame_bytes, kServerCap / 2);
    size_t idx = 0;   // cycles through bufs — 1 buffer = cache-HOT reuse, K buffers
                      // (K*frame > L3) = cache-COLD source each send.

    auto offer = [&]() -> bool {
        // Client-driven backpressure: never let the unsent backlog approach the
        // cap (which would drop the client). Wait for the drain to catch up.
        // NB: cli.bytes counts DELIVERED bytes INCLUDING the per-frame WS header,
        // while g_offered counts PAYLOAD only, so delivered can legitimately exceed
        // g_offered by the accumulated header bytes — compute backlog with an
        // underflow guard (else the unsigned subtraction wraps to a huge value and
        // the producer waits forever → three-way deadlock with the idle writer and
        // the blocked-in-recv client).
        auto backlog = [&]() -> uint64_t {
            uint64_t deliv = cli.bytes.load(std::memory_order_relaxed);
            return g_offered > deliv ? g_offered - deliv : 0;
        };
        auto wait_start = clk::now();
        while (backlog() > guard) {
            if (!cli.connected.load(std::memory_order_acquire)) return false;
            if (clk::now() - wait_start > std::chrono::seconds(5)) return false;  // stalled — bail
            std::this_thread::yield();
        }
        const std::vector<uint8_t>& frame = bufs[idx];
        if (bufs.size() > 1) idx = (idx + 1) % bufs.size();
        xi::ws::BinSpan seg{ frame.data(), frame.size() };
        bool ok = owned ? srv.send_binary_owned(&seg, 1, (void*)&frame, &noop_release)
                        : srv.send_binary(frame.data(), frame.size());
        if (ok) g_offered += frame_bytes;
        return ok;
    };

    // Warmup: fill the pipe and let the SNDBUF boost / page faults settle.
    auto t_warm_end = clk::now() + std::chrono::duration<double>(warmup_s);
    while (clk::now() < t_warm_end) {
        if (!offer()) return { 0, -1 };   // dropped during warmup
    }

    // Measured window: count delivered bytes at the client across it.
    uint64_t b0 = cli.bytes.load(std::memory_order_relaxed);
    auto t0 = clk::now();
    auto t_end = t0 + std::chrono::duration<double>(window_s);
    while (clk::now() < t_end) {
        if (!offer()) return { 0, -1 };
    }
    auto t1 = clk::now();
    uint64_t b1 = cli.bytes.load(std::memory_order_relaxed);

    double secs  = std::chrono::duration<double>(t1 - t0).count();
    double bytes = (double)(b1 - b0);
    double mbps  = bytes / (1024.0 * 1024.0) / secs;
    double fps   = (bytes / (double)frame_bytes) / secs;
    return { mbps, fps };
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered — see progress live
    xi::ws::Server srv;
    std::atomic<bool> opened{false};
    srv.on_open  = [&] { opened.store(true, std::memory_order_release); };
    if (!srv.start(0)) { std::fprintf(stderr, "server start failed\n"); return 1; }
    int port = srv.local_port();

    std::atomic<bool> poll_run{true};
    std::thread poll_th([&] { while (poll_run.load(std::memory_order_acquire)) srv.poll(10); });

    DrainClient cli;
    cli.start(port);

    // Wait for the connection to be fully up (server on_open + client past handshake).
    auto deadline = clk::now() + std::chrono::seconds(5);
    while ((!opened.load(std::memory_order_acquire) ||
            !cli.connected.load(std::memory_order_acquire)) && clk::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!opened.load() || !cli.connected.load()) {
        std::fprintf(stderr, "client did not connect\n");
        poll_run.store(false); poll_th.join(); cli.join(); return 1;
    }

    // Resolutions: match doc 32 exactly for apples-to-apples (2448x2048, 5120x3840).
    const size_t k5MP  = (size_t)2448 * 2048;   // 5,013,504 px
    const size_t k20MP = (size_t)5120 * 3840;   // 19,660,800 px
    std::vector<Scenario> scen = {
        { "5MP  mono8 (4.78 MB)",  k5MP  * 1 },
        { "5MP  RGB   (14.34 MB)", k5MP  * 3 },
        { "20MP mono8 (18.75 MB)", k20MP * 1 },
        { "20MP RGB   (56.25 MB)", k20MP * 3 },
    };

    // Cold working-set target: enough distinct buffers that the source can't stay
    // resident in CPU cache between reuses (L3 is tens of MB). 96 MiB comfortably
    // exceeds any desktop L3, so a K-buffer cycle reads cold memory each send.
    const size_t kColdWorkingSet = 96ull * 1024 * 1024;

    auto fill = [](std::vector<uint8_t>& b, uint8_t salt) {
        for (size_t i = 0; i < b.size(); i += 4096) b[i] = (uint8_t)((i >> 12) ^ salt);
    };

    std::printf("\n== WS raw-frame egress ceiling (loopback, in-process raw drain) ==\n");
    std::printf("box: this machine | port %d | window 2.5s + 0.5s warmup per run\n", port);
    std::printf("HOT = one reused buffer (cache-resident) | COLD = cycle K buffers, K*frame>96MiB (cache-cold source)\n\n");
    std::printf("%-24s | %-16s | %-16s | %-16s\n",
                "frame", "OWNED hot", "OWNED cold", "COPY hot");
    std::printf("%-24s-+-%-16s-+-%-16s-+-%-16s\n",
                "------------------------", "----------------", "----------------", "----------------");

    for (const auto& s : scen) {
        // HOT: a single reused buffer.
        std::vector<std::vector<uint8_t>> hot(1);
        hot[0].resize(s.frame_bytes);
        fill(hot[0], 0);

        // COLD: K distinct buffers whose total exceeds cache.
        size_t K = (size_t)std::max<uint64_t>(2, (kColdWorkingSet + s.frame_bytes - 1) / s.frame_bytes);
        std::vector<std::vector<uint8_t>> cold(K);
        for (size_t k = 0; k < K; ++k) { cold[k].resize(s.frame_bytes); fill(cold[k], (uint8_t)(k + 1)); }

        std::fprintf(stderr, "[%s] owned-hot..", s.name); std::fflush(stderr);
        Measure oh = blast(srv, cli, hot,  /*owned=*/true,  0.5, 2.5);
        std::fprintf(stderr, "owned-cold.."); std::fflush(stderr);
        Measure oc = blast(srv, cli, cold, /*owned=*/true,  0.5, 2.5);
        std::fprintf(stderr, "copy-hot.."); std::fflush(stderr);
        Measure ch = blast(srv, cli, hot,  /*owned=*/false, 0.5, 2.5);
        std::fprintf(stderr, "done\n"); std::fflush(stderr);

        auto fmt = [](char* out, size_t n, Measure m) {
            if (m.fps < 0) std::snprintf(out, n, "DROPPED");
            else std::snprintf(out, n, "%5.0f MB/s %5.1ffps", m.mbps, m.fps);
        };
        char c1[48], c2[48], c3[48];
        fmt(c1, sizeof(c1), oh); fmt(c2, sizeof(c2), oc); fmt(c3, sizeof(c3), ch);
        std::printf("%-24s | %-16s | %-16s | %-16s\n", s.name, c1, c2, c3);
    }
    std::printf("\n");

    poll_run.store(false);
    if (poll_th.joinable()) poll_th.join();
    cli.join();
    srv.stop();
    return 0;
}
