//
// bench_image_pool.cpp — measure ImagePool create/release throughput so we can
// decide whether the per-lifecycle PoolEntry+pixels allocation (release deletes
// the buffer; create news a fresh one) is worth turning into a buffer pool.
//
// Reports ns/create and creates/sec for a typical 320x240x3 image, single- and
// multi-threaded (the multi-threaded number is what high-parallelism dispatch
// would hit — allocator + lock-free slot-list contention). Not a correctness
// test; prints numbers + always exits 0.
//
// FINDING (2026-06, 20-core Win box) — buffer-reuse is NOT worth it:
//   - pure pool+alloc overhead (16x16): ~295 ns/create = ~3.4M/s single, still
//     ~1.8M/s under 16 threads (negative scaling = atomic/alloc contention, but
//     the aggregate dwarfs any real dispatch rate);
//   - a 230 KB frame: ~6.5 us/create (155K/s single, 740K/s @16thr) — dominated by
//     the memset / first-touch PAGE FAULTS, not malloc bookkeeping;
//   - a 1.3 MB frame: ~310 us/create — pure memory bandwidth.
//   At realistic rates (hundreds–low-thousands of inspects/s) the pool is <1% CPU.
//   Reuse would only save malloc + first-touch faults (the memset happens anyway),
//   invisible at our scale — not worth the risk of reworking a lock-free structure.
//   Re-run this if a future workload pushes >100K images/s.
//
// SUPERSEDED (2026-07, perf/imagepool-sizeclass): the hotpath-perf scan (C-1)
// flagged MB-scale per-frame frames as normal, and the design-C prototype
// proved a size-class magazine removes the alloc+zero-fill+fault cost. The
// pool now recycles pixel buffers (see pixpool in xi_image_pool.hpp); the
// 1920x1200 sections below measure the recycle path directly.
//
#include <xi/xi_image_pool.hpp>

#include "perf_fingerprint.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using clk = std::chrono::steady_clock;

static double run(int W, int H, int C, int threads, long iters_per_thread,
                  bool touch = true) {
    auto& pool = xi::ImagePool::instance();
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    auto t0 = clk::now();   // reassigned after the barrier
    std::atomic<long> done{0};
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {}
            for (long i = 0; i < iters_per_thread; ++i) {
                xi_image_handle h = pool.create(W, H, C);
                if (h) {
                    if (touch) {
                        uint8_t* p = pool.data(h);
                        if (p) std::memset(p, (int)(i & 0xff), (size_t)W * H * C);  // touch the buffer
                    }
                    pool.release(h);
                }
            }
            done.fetch_add(1, std::memory_order_relaxed);
        });
    }
    t0 = clk::now();
    go.store(true, std::memory_order_release);
    for (auto& th : ts) th.join();
    double secs = std::chrono::duration<double>(clk::now() - t0).count();
    long total = (long)threads * iters_per_thread;
    double per_sec = total / secs;
    double ns = secs * 1e9 / total;
    std::printf("  %4dx%-4d c%d  threads=%2d  %10.0f creates/s  %7.1f ns/create  (%ld in %.3fs)\n",
                W, H, C, threads, per_sec, ns, total, secs);
    std::fflush(stdout);
    return per_sec;
}

// --- perf-gate mode -------------------------------------------------------
// Single-threaded, best-of-R measurement (min strips scheduler noise) emitted
// as machine-readable INTEGER nanoseconds for tests/perf_gate.cmake. Integer
// units let the gate compare against a baseline with pure integer CMake math.
static long long measure_ns(int W, int H, int C, long iters) {
    auto& pool = xi::ImagePool::instance();
    auto t0 = clk::now();
    for (long i = 0; i < iters; ++i) {
        xi_image_handle h = pool.create(W, H, C);
        if (h) {
            uint8_t* p = pool.data(h);
            if (p) std::memset(p, (int)(i & 0xff), (size_t)W * H * C);
            pool.release(h);
        }
    }
    double secs = std::chrono::duration<double>(clk::now() - t0).count();
    return (long long)(secs * 1e9 / iters + 0.5);
}
static long long best_ns(int W, int H, int C, long iters, int R) {
    measure_ns(W, H, C, iters);                       // warm up
    long long best = (long long)1e18;
    for (int r = 0; r < R; ++r) {
        long long n = measure_ns(W, H, C, iters);
        if (n < best) best = n;
    }
    return best;
}
static int gate_main() {
    // Tiny isolates pool/slot+alloc overhead; the typical CV frame is the
    // realistic per-inspect cost. Both reported; either regressing trips the gate.
    long long tiny  = best_ns(16, 16, 1, 200000, 8);
    long long frame = best_ns(320, 240, 3, 20000, 8);
    std::printf("GATE image_pool_ns_per_create_16x16x1 %lld\n", tiny);
    std::printf("GATE image_pool_ns_per_create_320x240x3 %lld\n", frame);
    xi_perf::print_fingerprint();
    return 0;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--gate") == 0) return gate_main();

    std::printf("ImagePool create/release throughput (memset full buffer each iter):\n");
    std::fflush(stdout);
    run(320, 240, 3, 1, 5000);   // warm up
    std::printf("--- 320x240x3 (~230 KB, a typical CV frame) ---\n");
    for (int t : {1, 2, 4, 8, 16}) run(320, 240, 3, t, 20000);
    std::printf("--- 1280x1024x1 (~1.3 MB) ---\n");
    for (int t : {1, 4, 8}) run(1280, 1024, 1, t, 5000);
    std::printf("--- 1920x1200x1 (~2.3 MB, the size-class recycle target) ---\n");
    for (int t : {1, 4}) run(1920, 1200, 1, t, 2000);
    std::printf("--- 1920x1200x1 create/release only (no caller memset — pure alloc path) ---\n");
    for (int t : {1, 4}) run(1920, 1200, 1, t, 2000, /*touch=*/false);
    std::printf("--- 16x16x1 (tiny — isolates pool/slot+alloc overhead from buffer size) ---\n");
    for (int t : {1, 8, 16}) run(16, 16, 1, t, 300000);
    return 0;
}
