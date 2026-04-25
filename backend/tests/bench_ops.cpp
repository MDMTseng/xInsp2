//
// bench_ops.cpp — quantify xi::ops {toGray, threshold, gaussian, sobel,
// erode, dilate} on a 1920×1080 frame. With XINSP2_HAS_IPP defined the
// IPP-accelerated path runs; without, the portable C++ falls through.
// Same binary either way — link decides.
//

#include <xi/xi_image.hpp>
#include <xi/xi_ops.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char* mode_label() {
#if defined(XINSP2_HAS_IPP) && defined(XINSP2_HAS_OPENCV)
    return "ipp > opencv > cpp";
#elif defined(XINSP2_HAS_IPP)
    return "ipp > cpp";
#elif defined(XINSP2_HAS_OPENCV)
    return "opencv > cpp";
#else
    return "cpp only";
#endif
}

static xi::Image make_rgb(int w, int h) {
    xi::Image img(w, h, 3);
    auto* p = img.data();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            p[(y*w + x) * 3 + 0] = (uint8_t)((x * 255) / w);
            p[(y*w + x) * 3 + 1] = (uint8_t)((y * 255) / h);
            p[(y*w + x) * 3 + 2] = (uint8_t)(((x + y) * 255) / (w + h));
        }
    return img;
}

template <class F>
static double time_ms(int iters, F&& fn) {
    // Warm up
    for (int i = 0; i < 2; ++i) fn();
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) fn();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
}

int main(int argc, char** argv) {
    int iters = (argc > 1) ? std::atoi(argv[1]) : 30;
    int w     = (argc > 2) ? std::atoi(argv[2]) : 1920;
    int h     = (argc > 3) ? std::atoi(argv[3]) : 1080;

    auto rgb  = make_rgb(w, h);
    auto gray = xi::ops::toGray(rgb);

    std::printf("mode:   %s\n", mode_label());
    std::printf("image:  %dx%d (%.2f MP)  iters=%d\n",
                w, h, (double)w*h/1e6, iters);
    std::printf("%-26s %10s %14s\n", "op", "ms/call", "MP/s");

    auto report = [&](const char* name, double ms) {
        double mp = (double)w * h / 1e6;
        std::printf("%-26s %10.2f %14.1f\n", name, ms, mp / (ms / 1000.0));
        std::fflush(stdout);
    };

    {
        auto ms = time_ms(iters, [&]{ volatile auto v = xi::ops::toGray(rgb);  (void)v; });
        report("toGray (RGB→Gray)", ms);
    }
    {
        auto ms = time_ms(iters, [&]{ volatile auto v = xi::ops::threshold(gray, 128); (void)v; });
        report("threshold(t=128)", ms);
    }
    {
        auto ms = time_ms(iters, [&]{ volatile auto v = xi::ops::gaussian(gray, 3); (void)v; });
        report("gaussian(r=3)", ms);
    }
    {
        auto ms = time_ms(iters, [&]{ volatile auto v = xi::ops::sobel(gray); (void)v; });
        report("sobel", ms);
    }
    {
        auto ms = time_ms(iters, [&]{ volatile auto v = xi::ops::erode(gray, 1); (void)v; });
        report("erode(r=1)", ms);
    }
    {
        auto ms = time_ms(iters, [&]{ volatile auto v = xi::ops::dilate(gray, 1); (void)v; });
        report("dilate(r=1)", ms);
    }
    return 0;
}
