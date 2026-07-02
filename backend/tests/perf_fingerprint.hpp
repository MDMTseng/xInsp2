//
// perf_fingerprint.hpp — shared perf-gate environment fingerprint emitter.
//
// Each perf bench, in --gate mode, prints a set of `FP <key> <value>` lines
// describing the environment class it was built/run in. perf_gate.cmake reads
// these, MERGES host facts it knows (cpu / logical_cores / os), and either
//   - writes them as the `# xi.perf-baseline/1` header (UPDATE_BASELINE=ON), or
//   - compares them against the committed baseline's header and, on an
//     environment-class mismatch, SKIPS the gate with an explicit reason
//     instead of reporting a false regression.
//
// The bench owns the facts only it can see at compile time: which JPEG encoder
// was actually wired in, the OpenCV version, the build type, and the xInsp
// commit. Everything here is a compile-time constant or a trivial print — no
// runtime cost that could perturb the measurement (fingerprint is emitted
// AFTER all timing).
//
#pragma once

#include <cstdio>

#ifndef XINSP2_COMMIT
#define XINSP2_COMMIT "unknown"
#endif

// OpenCV version is only knowable when the bench actually links OpenCV.
#if defined(XINSP2_HAS_OPENCV) && __has_include(<opencv2/core/version.hpp>)
#include <opencv2/core/version.hpp>
#define XI_PERF_OPENCV_VER CVAUX_STR(CV_VERSION_MAJOR) "." CVAUX_STR(CV_VERSION_MINOR) "." CVAUX_STR(CV_VERSION_REVISION)
#else
#define XI_PERF_OPENCV_VER "none"
#endif

namespace xi_perf {

// The active JPEG-encoder backend class. Baselines are only comparable WITHIN
// the same backend (turbojpeg vs opencv vs ipp vs stb differ by 2-5x), so this
// also feeds the backend-aware JPEG metric key.
inline const char* jpeg_backend() {
#if defined(XINSP2_HAS_TURBOJPEG)
    return "turbojpeg";
#elif defined(XINSP2_HAS_IPP)
    return "ipp";
#elif defined(XINSP2_HAS_OPENCV)
    return "opencv";
#else
    return "stb";
#endif
}

inline const char* build_type() {
#if defined(NDEBUG)
    return "release";
#else
    return "debug";
#endif
}

// Emit the fingerprint facts the bench owns. Host facts (cpu/cores/os) are
// added by perf_gate.cmake. Call this LAST, after all timing, in --gate mode.
inline void print_fingerprint() {
    std::printf("FP schema xi.perf-baseline/1\n");
    std::printf("FP jpeg_backend %s\n", jpeg_backend());
    std::printf("FP opencv %s\n", XI_PERF_OPENCV_VER);
    std::printf("FP build_type %s\n", build_type());
    std::printf("FP xinsp_commit %s\n", XINSP2_COMMIT);
    std::fflush(stdout);
}

}  // namespace xi_perf
