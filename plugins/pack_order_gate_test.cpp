//
// pack_order_gate_test.cpp — U3 (docs/new_gen/17): frame-order delivery of
// staged pack pushes under PARALLEL dispatch, asserted at the sink's door.
//
// The contract under test (doc 17 §1, the envelope half): for a declared
// ordered sink, use(sink).push(pack) is staged on the worker and flushed
// inside the EmitTurn gate — so the sink's xi.pack@1 door observes pushes in
// FRAME-ARRIVAL order (and, within one frame, in script call order) even when
// compute finishes wildly out of order. No pack entry is needed for this:
// the producer-stamped $seq rides along untouched and is what we READ BACK off
// the emitted XEX1 wire frames to prove the door-observed order.
//
// Harness shape: the REAL ordering primitive (xi::EmitGate/EmitTurn — the
// exact gate run_one_inspection claims) + the REAL expose DLL door driven
// under CAbiInstanceAdapter, with the service's use_push_pack_cb staging +
// flush_staged_emits_ discipline mirrored per worker (the service cb itself is
// not linkable outside the backend binary — the same test seam as
// expose_script_push_test.cpp).
//
//   * ARRIVAL (gated): 4 workers, uneven compute (every 5th frame slow), each
//     frame claims its emit seq at "dequeue" (in arrival order), computes in
//     parallel, stages TWO pushes ($seq = 2*run, 2*run+1 — a strictly
//     increasing wire sequence iff BOTH frame order and within-frame call
//     order hold), then flushes under wait_turn(). Assert: the door-emitted
//     wire seqs are exactly 2,3,...,2N+1 — zero inversions.
//   * COMPLETION (ungated control): the same workload with a null gate
//     (EmitTurn seq -1 — the service's completion mode). Assert: inversions
//     observed (> 0), proving the workload genuinely reorders and the gate —
//     not scheduling luck — is what ordered phase A.
//   * Balance: PackRegistry live frames + ImagePool live handles return to
//     baseline (the staged retain/release discipline leaks nothing).
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door)
#include <xi/xi_emit_gate.hpp>      // the REAL ordered-emit primitive (EmitGate/EmitTurn)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_binary_sink.hpp>    // capture emit_binary (the plugin -> WS byte pipe)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef EXPOSE_DLL_PATH
#define EXPOSE_DLL_PATH "xi-expose.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// --- capture the plugin's emit_binary off the host sink ---------------------
// Mutex-guarded: the ungated (completion) phase flushes from 4 workers
// concurrently. (In the gated phase the EmitTurn already serializes.)
static std::mutex g_emit_mu;
static std::vector<std::vector<uint8_t>> g_emitted;
static void capture_binary(const void* data, int len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::lock_guard<std::mutex> lk(g_emit_mu);
    g_emitted.emplace_back(p, p + (len > 0 ? len : 0));
}

// Decode the top-level `seq` off an XEX1-v1 frame: the frame is 'XEX1' + a
// msgpack map whose fixed key order is v, channel, seq, ... — so the FIRST
// fixstr(3) "seq" in the byte stream is the top-level field; the value that
// follows is a msgpack uint (fixint / uint8 / 16 / 32 / 64).
static long long wire_seq(const std::vector<uint8_t>& f) {
    for (size_t i = 4; i + 4 < f.size(); ++i) {
        if (f[i] == 0xA3 && f[i + 1] == 's' && f[i + 2] == 'e' && f[i + 3] == 'q') {
            size_t v = i + 4;
            uint8_t b = f[v];
            if (b <= 0x7F) return b;
            auto be = [&](int n) {
                long long x = 0;
                for (int k = 1; k <= n; ++k) x = (x << 8) | f[v + k];
                return x;
            };
            if (b == 0xCC) return be(1);
            if (b == 0xCD) return be(2);
            if (b == 0xCE) return be(4);
            if (b == 0xCF) return be(8);
            return -1;
        }
    }
    return -1;
}

static long long inversions(const std::vector<long long>& s) {
    long long inv = 0;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i] < s[i - 1]) ++inv;
    return inv;
}

// --- the workload: N frames, 4 workers, every 5th frame slow ----------------
// Mirrors the service exactly where it matters:
//   * the emit seq is CLAIMED in arrival order (atomic fetch = the dequeue
//     under the lane lock),
//   * compute runs in parallel and finishes out of order,
//   * each staged push holds its own retained pack ref (use_push_pack_cb),
//   * the flush runs the door + drops the ack + releases the staged ref, in
//     staging order, under the EmitTurn (flush_staged_emits_ inside the gate).
// `gate == nullptr` is the completion-mode control (EmitTurn seq -1: no-op).
static void run_workload(xi::CAbiInstanceAdapter* sink, const xi_pack_v1* fi,
                         int frames, xi::EmitGate* gate) {
    std::atomic<long long> next{0};
    auto worker = [&] {
        for (;;) {
            long long run = next.fetch_add(1) + 1;   // claim in arrival order
            if (run > frames) return;
            xi::EmitTurn turn(gate, gate ? run - 1 : -1);   // claimed BEFORE compute

            // Parallel compute, deliberately uneven so completion order != run order.
            std::this_thread::sleep_for(std::chrono::milliseconds(run % 5 == 0 ? 30 : 1));

            // Stage TWO pushes (script call order within the frame): $seq 2*run,
            // 2*run+1. Producer-stamped identity, per the doc-17 contract; the
            // staged retain is the service's "our ref outlives the script's".
            std::vector<xi_pack_handle> staged;
            for (int k = 0; k < 2; ++k) {
                xi_pack_builder b = fi->builder_new();
                fi->builder_add_str(b, "$channel", "order", 5);
                fi->builder_add_i64(b, "$seq", 2 * run + k);
                fi->builder_add_i64(b, "run", run);
                xi_pack_handle h = fi->builder_seal(b);
                if (h == XI_PACK_NULL) { ++g_failures; continue; }
                staged.push_back(h);   // seal ref doubles as the staged ref here
            }

            // The flush: inside the gate, in staging order, ack dropped,
            // staged ref released — flush_staged_emits_'s pack branch.
            turn.wait_turn();
            for (xi_pack_handle h : staged) {
                xi_pack_handle ack = sink->run_pack_door(h);
                if (ack != XI_PACK_NULL) xi::PackRegistry::instance().release(ack);
                xi::PackRegistry::instance().release(h);
            }
            turn.complete();
        }
    };
    std::vector<std::thread> pool;
    for (int t = 0; t < 4; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
}

int main() {
    std::printf("[test] U3 pack ordering — staged push flush under the EmitTurn gate (doc 17)\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    xi::binary_sink() = &capture_binary;

    const int    base_live   = xi::ImagePool::instance().cumulative().live_now;
    const size_t base_frames = xi::PackRegistry::instance().live_frames();

    HMODULE dll = LoadLibraryA(EXPOSE_DLL_PATH);
    if (!dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", EXPOSE_DLL_PATH, GetLastError()); return 1; }
    auto factory = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    CHECK(factory != nullptr);
    if (!factory) return 1;
    void* inst = factory(&host, "sink0");
    CHECK(inst != nullptr);
    if (!inst) return 1;
    auto expose = std::make_unique<xi::CAbiInstanceAdapter>(
        "sink0", "expose", dll, inst, /*reentrant=*/false, /*max_concurrency=*/0,
        /*is_sink=*/true);
    CHECK(expose->has_pack_door());
    expose->exchange("{\"command\":\"subscribe\",\"channels\":[\"order\"]}");

    constexpr int kFrames = 48;

    // -----------------------------------------------------------------------
    SECTION("arrival: gated flush -> door-observed wire seq strictly in frame order");
    {
        g_emitted.clear();
        xi::EmitGate gate;
        run_workload(expose.get(), fi, kFrames, &gate);

        std::vector<long long> seqs;
        for (auto& f : g_emitted) seqs.push_back(wire_seq(f));
        CHECK((int)seqs.size() == 2 * kFrames);
        CHECK(inversions(seqs) == 0);
        // Exactly 2, 3, ..., 2N+1: frame order AND within-frame call order.
        bool exact = (int)seqs.size() == 2 * kFrames;
        for (size_t i = 0; exact && i < seqs.size(); ++i)
            exact = seqs[i] == (long long)(i + 2);
        CHECK(exact);
        std::printf("       frames=%zu inversions=%lld (expect 0)\n",
                    seqs.size(), inversions(seqs));
    }

    // -----------------------------------------------------------------------
    SECTION("completion control: ungated flush on the same workload -> reorders");
    {
        g_emitted.clear();
        run_workload(expose.get(), fi, kFrames, nullptr);

        std::vector<long long> seqs;
        {
            std::lock_guard<std::mutex> lk(g_emit_mu);
            for (auto& f : g_emitted) seqs.push_back(wire_seq(f));
        }
        long long inv = inversions(seqs);
        CHECK((int)seqs.size() == 2 * kFrames);
        // The point of the control: the workload REALLY completes out of order,
        // so phase A's zero is the gate's doing, not scheduling luck.
        CHECK(inv > 0);
        std::printf("       frames=%zu inversions=%lld (expect > 0)\n", seqs.size(), inv);
    }

    // ---- teardown + pool/registry balance (the staged-ref leak oracle) ----
    xi::binary_sink() = nullptr;
    expose.reset();          // ~CAbiInstanceAdapter -> xi_plugin_destroy + leak sweep
    FreeLibrary(dll);

    CHECK(xi::ImagePool::instance().cumulative().live_now == base_live);
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
