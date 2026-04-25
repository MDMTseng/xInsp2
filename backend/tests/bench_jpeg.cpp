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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

static const char* dispatch_label() {
#ifdef XINSP2_HAS_IPP
    return "ipp";
#elif defined(XINSP2_HAS_OPENCV)
    return "opencv";
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

int main(int argc, char** argv) {
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
