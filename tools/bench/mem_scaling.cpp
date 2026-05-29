// mem_scaling.cpp — memory-bandwidth / cache-contention scaling probe.
//
// Models the xInsp2 "should I run dispatch_threads > 1 for large
// images?" question. Each thread owns its OWN large byte array and
// does a fixed number of RANDOM read-modify-write accesses into it.
// We sweep thread counts and report aggregate throughput + parallel
// efficiency.
//
// Interpretation:
//   - efficiency stays ~100%  → compute/cache-friendly; threads scale
//   - efficiency drops toward 1/T → DRAM-bandwidth-bound; extra threads
//                                    fight for the same memory bus and
//                                    give little (or net-negative) gain
//
// Random access defeats the prefetcher, so each access is ~a cache
// miss for an array that doesn't fit in L2/L3 — exactly the regime a
// 20 MP+ image lands in.
//
// Build (from repo root, in a VS dev shell or via vcvars64.bat):
//   cl /O2 /EHsc /std:c++17 tools\bench\mem_scaling.cpp /Fe:mem_scaling.exe
// Run:
//   mem_scaling.exe [array_MB=20] [accesses_M_per_thread=30] [max_threads=auto]

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// xorshift64* — fast per-thread PRNG, zero shared state.
static inline uint64_t xs(uint64_t& s) {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 0x2545F4914F6CDD1Dull;
}

struct ThreadResult {
    double   seconds   = 0.0;
    uint64_t checksum  = 0;   // anti-optimization sink
};

// mode: 0 = random RMW, 1 = sequential read, 2 = sequential RMW
static void worker(size_t bytes, uint64_t accesses, uint64_t seed,
                   int mode, std::atomic<int>* go, ThreadResult* out) {
    // Each thread allocates + faults in its OWN array.
    std::vector<uint8_t> arr(bytes, 0);
    for (size_t i = 0; i < bytes; i += 4096) arr[i] = (uint8_t)(i & 0xFF);

    uint64_t s = seed ? seed : 0x9E3779B97F4A7C15ull;

    // For sequential modes, `accesses` is the total byte-touch budget;
    // we convert it to whole passes over the array so prefetch can
    // stream cleanly.
    uint64_t passes = (bytes > 0) ? (accesses + bytes - 1) / bytes : 1;
    if (passes == 0) passes = 1;

    // Spin until the launcher releases all threads together, so they
    // contend for the memory bus simultaneously (worst case).
    while (go->load(std::memory_order_acquire) == 0) { /* spin */ }

    auto t0 = std::chrono::steady_clock::now();
    uint64_t acc = 0;
    if (mode == 0) {
        // Random read-modify-write — defeats prefetch, ~cache miss/access.
        for (uint64_t i = 0; i < accesses; ++i) {
            size_t idx = (size_t)(xs(s) % bytes);
            arr[idx] = (uint8_t)(arr[idx] + (uint8_t)(acc + 1));
            acc += arr[idx];
        }
    } else if (mode == 1) {
        // Sequential read — full prefetch + SIMD; pure read bandwidth.
        for (uint64_t p = 0; p < passes; ++p) {
            for (size_t i = 0; i < bytes; ++i) acc += arr[i];
        }
    } else {
        // Sequential read-modify-write — streaming write bandwidth too.
        for (uint64_t p = 0; p < passes; ++p) {
            for (size_t i = 0; i < bytes; ++i) {
                arr[i] = (uint8_t)(arr[i] + 1);
                acc += arr[i];
            }
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    out->seconds  = std::chrono::duration<double>(t1 - t0).count();
    out->checksum = acc;
}

int main(int argc, char** argv) {
    size_t   array_mb     = (argc > 1) ? (size_t)std::strtoull(argv[1], nullptr, 10) : 20;
    uint64_t accesses_m   = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 30;
    unsigned hw           = std::thread::hardware_concurrency();
    unsigned max_threads  = (argc > 3) ? (unsigned)std::strtoul(argv[3], nullptr, 10)
                                       : (hw ? hw : 8);
    int      mode         = (argc > 4) ? (int)std::strtol(argv[4], nullptr, 10) : 0;

    const size_t   bytes    = array_mb * 1024ull * 1024ull;
    const uint64_t accesses = accesses_m * 1000000ull;
    const char* mode_name = (mode == 0) ? "random RMW"
                          : (mode == 1) ? "sequential read (prefetch + SIMD)"
                          :               "sequential RMW";

    std::printf("mem_scaling — %s over per-thread %zu MB arrays\n", mode_name, array_mb);
    std::printf("  hardware_concurrency = %u\n", hw);
    std::printf("  byte-touch budget/thread = %lluM\n", (unsigned long long)accesses_m);
    std::printf("  array/thread         = %zu MB\n\n", array_mb);

    // Sweep thread counts: 1, 2, 4, 8, ... up to max_threads.
    std::vector<unsigned> sweep;
    for (unsigned t = 1; t <= max_threads; t *= 2) sweep.push_back(t);
    if (sweep.empty() || sweep.back() != max_threads) sweep.push_back(max_threads);

    std::printf("%-8s %-12s %-16s %-16s %-12s\n",
                "threads", "wall(s)", "agg Macc/s", "per-thr Macc/s", "efficiency");
    std::printf("------------------------------------------------------------------------\n");

    double base_per_thread = 0.0;  // single-thread per-thread throughput

    for (unsigned T : sweep) {
        std::vector<std::thread>   threads;
        std::vector<ThreadResult>  results(T);
        std::atomic<int> go{0};
        threads.reserve(T);
        for (unsigned i = 0; i < T; ++i) {
            threads.emplace_back(worker, bytes, accesses,
                                 0x100000001b3ull * (i + 1) + 0xABCDEFull,
                                 mode, &go, &results[i]);
        }
        // Give threads a moment to allocate + fault-in their arrays,
        // then release them all at once.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto wall0 = std::chrono::steady_clock::now();
        go.store(1, std::memory_order_release);
        for (auto& th : threads) th.join();
        auto wall1 = std::chrono::steady_clock::now();

        double wall = std::chrono::duration<double>(wall1 - wall0).count();
        uint64_t total_acc = accesses * T;
        double   agg_macc  = (double)total_acc / wall / 1e6;
        double   per_thr   = agg_macc / T;
        if (T == 1) base_per_thread = per_thr;
        double eff = base_per_thread > 0 ? (per_thr / base_per_thread) * 100.0 : 100.0;

        // Sink the checksum so the loop can't be optimized away.
        volatile uint64_t sink = 0;
        for (auto& r : results) sink += r.checksum;
        (void)sink;

        std::printf("%-8u %-12.3f %-16.1f %-16.1f %-11.0f%%\n",
                    T, wall, agg_macc, per_thr, eff);
    }

    std::printf("\nReading the table:\n");
    std::printf("  efficiency ~100%%  → threads scale (compute/cache-bound)\n");
    std::printf("  efficiency -> 1/T → DRAM-bandwidth-bound; extra threads\n");
    std::printf("                       just split the same memory bus\n");
    return 0;
}
