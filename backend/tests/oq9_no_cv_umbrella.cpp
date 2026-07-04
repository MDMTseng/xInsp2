// OQ-9 acceptance proof — a NO-CV translation unit.
//
// It includes ONLY <xi/xi.hpp> (the mandatory umbrella) and does no OpenCV.
// The whole point of OQ-9: this MUST compile with the OpenCV include dir
// REMOVED from the command line. If xi.hpp (or anything it pulls) dragged in
// <opencv2/...>, this file would fail to find the header and not compile.
//
// Build (note: NO /I for OpenCV):
//   cl /std:c++20 /EHsc /c /I backend/include /I backend/vendor \
//      backend/tests/oq9_no_cv_umbrella.cpp
//
#include <xi/xi.hpp>

#include <cstdint>
#include <cstdio>

int main() {
    // --- Image: allocate, write a pixel, read it back (raw buffer only) ---
    xi::Image img(4, 3, 1);                    // 4x3 gray, heap-backed
    if (img.empty() || img.data() == nullptr) return 1;
    img.data()[0]              = 200;          // write via raw pointer
    img.data()[img.stride()]   = 55;           // row 1, col 0
    const uint8_t px = img.data()[0];          // read a pixel
    if (px != 200) return 2;
    if (img.width != 4 || img.height != 3 || img.channels != 1) return 3;
    if (img.stride() != 4) return 4;

    // --- Param: a tunable, read its value (no CV) ---
    xi::Param<double> thresh("thresh", 128.0, {0.0, 255.0});
    double t = thresh.get();
    if (t != 128.0) return 5;

    // THE CUT (v12): the xi::Record result type was deleted and is no longer in
    // the <xi/xi.hpp> umbrella (the pack data plane lives in xi_pack.hpp, outside
    // the umbrella). Image + Param above already prove the umbrella compiles with
    // OpenCV removed from the command line, which is all OQ-9 asserts.

    std::printf("oq9 no-cv umbrella OK: px=%u thresh=%.1f\n",
                (unsigned)px, t);
    return 0;
}
