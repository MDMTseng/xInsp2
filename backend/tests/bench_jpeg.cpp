//
// bench_jpeg.cpp — quantify which JPEG encoder is wired in.
//
// Generates a synthetic 1920x1080 RGB gradient and encodes it N times
// with whatever path xi::encode_jpeg dispatches to (IPP > OpenCV > stb
// per CPU vendor + build flags). Prints encoder name, throughput, and
// output size, then exits.
//
// Usage: bench_jpeg [iterations] [width] [height]   (defaults 50 1920 1080)
//

#include <xi/xi_image.hpp>
#include <xi/xi_jpeg.hpp>

#include "perf_fingerprint.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

// Process CPU time (user+kernel) in seconds — "cores busy" = cpu/wall.
static double proc_cpu_seconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto to = [](FILETIME f) { ULARGE_INTEGER x; x.LowPart = f.dwLowDateTime;
        x.HighPart = f.dwHighDateTime; return double(x.QuadPart) * 1e-7; };
    return to(k) + to(u);
#else
    return 0.0;
#endif
}

static const char* dispatch_label() {
#ifdef XINSP2_HAS_TURBOJPEG
    return "libjpeg-turbo (direct, RGB)";
#elif defined(XINSP2_HAS_IPP)
    return "ipp";
#elif defined(XINSP2_HAS_OPENCV)
    return "opencv (libjpeg + cvtColor)";
#else
    return "stb";
#endif
}

static xi::Image make_gradient(int w, int h) {
    xi::Image img(w, h, 3);
    uint8_t* p = img.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Smooth RGB gradient — JPEG-friendly (DCT loves low-freq).
            p[(y * w + x) * 3 + 0] = (uint8_t)((x * 255) / w);
            p[(y * w + x) * 3 + 1] = (uint8_t)((y * 255) / h);
            p[(y * w + x) * 3 + 2] = (uint8_t)(((x + y) * 255) / (w + h));
        }
    }
    return img;
}

// --- perf-gate mode -------------------------------------------------------
// Fixed 1920x1080 workload, best-of-R per-encode time emitted as machine-
// readable INTEGER microseconds for tests/perf_gate.cmake. Whatever encoder
// the build dispatched to (turbojpeg/IPP/OpenCV/stb) is what gets baselined.
static int gate_main() {
    const int w = 1920, h = 1080;
    auto img = make_gradient(w, h);
    std::vector<uint8_t> jpeg;
    for (int i = 0; i < 5; ++i) (void)xi::encode_jpeg(img, 85, jpeg);   // warm up
    const int L = 15;
    double best_us = 1e30;
    for (int b = 0; b < 8; ++b) {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < L; ++i) (void)xi::encode_jpeg(img, 85, jpeg);
        auto t1 = std::chrono::steady_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / L;
        if (us < best_us) best_us = us;
    }
    std::printf("encoder: %s\n", dispatch_label());
    // Backend-aware metric key: turbojpeg/opencv/ipp/stb differ by 2-5x, so a
    // machine running a different encoder must NOT look like a regression.
    // The key carries the active backend; perf_gate.cmake also gates on the
    // baseline fingerprint's jpeg_backend, so cross-backend keys never even
    // meet (a foreign key is reported "no baseline — skipped").
    std::printf("GATE jpeg_%s_q85_1920x1080_us %lld\n",
                xi_perf::jpeg_backend(), (long long)(best_us + 0.5));
    xi_perf::print_fingerprint();
    return 0;
}

// --- multi-thread scaling mode -------------------------------------------
// Sweeps thread counts; each thread encodes the SAME (read-only) image into its
// OWN output buffer for a fixed wall window (encode_jpeg_turbo keeps a
// thread_local tj compressor, so N threads = N independent encoders). Reports
// aggregate throughput, CPU cores busy, and scaling efficiency vs 1 thread —
// i.e. how well CPU JPEG encode parallelises across cores.
static int mt_main(int w, int h, double secs) {
    auto img = make_gradient(w, h);
    const double mp = (double)w * h / 1e6;
    unsigned hw = std::thread::hardware_concurrency();
    if (!hw) hw = 8;
    std::vector<unsigned> Ts;
    for (unsigned t : {1u, 2u, 4u, 8u, 16u}) if (t <= hw) Ts.push_back(t);
    if (Ts.empty() || Ts.back() != hw) Ts.push_back(hw);

    std::printf("encoder: %s\n", dispatch_label());
    std::printf("image:   %dx%d (%.2f MP) q85 | %u logical CPUs | %.1fs window/point\n\n",
                w, h, mp, hw, secs);
    std::printf("%-8s | %-9s | %-10s | %-11s | %-7s | %-10s\n",
                "threads", "agg fps", "agg MP/s", "fps/thread", "cores", "efficiency");
    std::printf("---------+-----------+------------+-------------+---------+-----------\n");

    double base_fps = 0;
    for (unsigned T : Ts) {
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        std::chrono::steady_clock::time_point deadline;   // set before go (release/acquire)
        std::vector<uint64_t> counts(T, 0);
        std::vector<std::thread> pool;
        for (unsigned id = 0; id < T; ++id) {
            pool.emplace_back([&, id] {
                std::vector<uint8_t> out;
                (void)xi::encode_jpeg(img, 85, out);      // warm the thread_local tj handle
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) { /* spin to align start */ }
                uint64_t c = 0;
                while (std::chrono::steady_clock::now() < deadline) {
                    xi::encode_jpeg(img, 85, out);
                    ++c;
                }
                counts[id] = c;
            });
        }
        while (ready.load(std::memory_order_acquire) < T) { /* wait all warmed */ }
        deadline = std::chrono::steady_clock::now() +
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(secs));
        double cpu0 = proc_cpu_seconds();
        auto wall0 = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);
        for (auto& th : pool) th.join();
        double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
        double cores = (proc_cpu_seconds() - cpu0) / wall;
        uint64_t tot = 0; for (auto c : counts) tot += c;
        double agg_fps = tot / wall;
        if (base_fps == 0) base_fps = agg_fps;
        double eff = agg_fps / (base_fps * T);
        char effs[24];
        std::snprintf(effs, sizeof(effs), "%.0f%%%s", eff * 100.0, T == 1 ? " base" : "");
        std::printf("%-8u | %-9.0f | %-10.0f | %-11.0f | %-7.1f | %-10s\n",
                    T, agg_fps, agg_fps * mp, agg_fps / T, cores, effs);
    }
    return 0;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--gate") return gate_main();

    // --mt [W H] [secs]: multi-thread scaling sweep.
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--mt") {
            int    w    = (argc > i + 1) ? std::atoi(argv[i + 1]) : 2448;
            int    h    = (argc > i + 2) ? std::atoi(argv[i + 2]) : 2048;
            double secs = (argc > i + 3) ? std::atof(argv[i + 3]) : 2.0;
            if (w <= 0 || h <= 0 || secs <= 0) { std::fprintf(stderr, "usage: bench_jpeg --mt [W H] [secs]\n"); return 2; }
            return mt_main(w, h, secs);
        }
    }

    int iters = (argc > 1) ? std::atoi(argv[1]) : 50;
    int w     = (argc > 2) ? std::atoi(argv[2]) : 1920;
    int h     = (argc > 3) ? std::atoi(argv[3]) : 1080;
    if (iters <= 0 || w <= 0 || h <= 0) {
        std::fprintf(stderr, "usage: bench_jpeg [iterations] [width] [height]\n");
        return 2;
    }

    auto img = make_gradient(w, h);
    std::vector<uint8_t> jpeg;

    // Warm-up — dwarf the first-call lazy init.
    for (int i = 0; i < 3; ++i) (void)xi::encode_jpeg(img, 85, jpeg);

    auto t0 = std::chrono::steady_clock::now();
    size_t total_bytes = 0;
    int    failures = 0;
    for (int i = 0; i < iters; ++i) {
        if (!xi::encode_jpeg(img, 85, jpeg)) { ++failures; continue; }
        total_bytes += jpeg.size();
    }
    auto t1 = std::chrono::steady_clock::now();

    double ms_total = std::chrono::duration<double, std::milli>(t1 - t0).count();
    double ms_per   = ms_total / iters;
    double mp       = (double)w * h / 1e6;
    double mp_per_s = (mp * iters) / (ms_total / 1000.0);
    double avg_kb   = (double)total_bytes / iters / 1024.0;

    std::printf("encoder:        %s\n", dispatch_label());
    std::printf("image:          %dx%d  (%.2f MP)  3ch RGB\n", w, h, mp);
    std::printf("iterations:     %d\n", iters);
    std::printf("failures:       %d\n", failures);
    std::printf("total time:     %.1f ms\n", ms_total);
    std::printf("per-encode:     %.2f ms\n", ms_per);
    std::printf("throughput:     %.2f MP/s\n", mp_per_s);
    std::printf("avg JPEG size:  %.1f KB  (q=85)\n", avg_kb);
    std::printf("compression:    %.1fx vs raw RGB\n",
                (double)(w * h * 3) / (total_bytes / (double)iters));

    return failures > 0 ? 1 : 0;
}
