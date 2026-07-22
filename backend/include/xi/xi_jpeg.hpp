#pragma once
//
// xi_jpeg.hpp — encode xi::Image to JPEG bytes.
//
// polaris2 v12 (THE CUT): this is NO LONGER an in-core backend engine. The
// backend's compress path (compress_sink) is capability-only and never calls
// encode_jpeg. This header survives because the imgcodec LIB PLUGIN
// (toolbox/imgcodec, which keeps its OWN XINSP2_HAS_TURBOJPEG) consumes
// xi::encode_jpeg as its encoder, and bench_jpeg / cap_jpeg_encode_host_test
// reference it. It is NOT deleted; it is simply no longer wired into the
// backend target. Do not add new backend callers.
//
// This header is OpenCV-FREE (core-sheds-OpenCV, 2026-07). The OpenCV imencode
// path (and the CPU-vendor probe that only existed to choose between it and
// turbo) was removed: image processing / OpenCV belongs to plugins & scripts via
// the opt-in <xi/xi_cv.hpp>, not to a header the core links. Encoders, fastest
// first:
//   - XINSP2_HAS_TURBOJPEG: libjpeg-turbo (the fast SIMD path; direct RGB)
//   - Fallback:             stb_image_write (no deps, slowest)
//

#include <cstdint>
#include <vector>

#include "xi_image.hpp"

#ifdef XINSP2_HAS_TURBOJPEG
  #include <turbojpeg.h>
#endif

// stb forward-declare (always available as fallback)
extern "C" int stbi_write_jpg_to_func(
    void (*func)(void* context, void* data, int size),
    void* context,
    int x, int y, int comp, const void* data, int quality);

namespace xi {

// ---------- stb fallback ----------

inline bool encode_jpeg_stb(const Image& img, int quality, std::vector<uint8_t>& out) {
    if (img.empty()) return false;
    if (img.channels != 1 && img.channels != 3 && img.channels != 4) return false;
    out.clear();
    auto writer = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<uint8_t>*>(ctx);
        auto* p = static_cast<uint8_t*>(data);
        v->insert(v->end(), p, p + size);
    };
    int ok = stbi_write_jpg_to_func(writer, &out,
                                     img.width, img.height, img.channels,
                                     img.data(), quality);
    return ok != 0;
}

// ---------- libjpeg-turbo (direct) ----------
//
// Calls tjCompress2 with TJPF_RGB so xi::Image's native RGB layout flows
// straight into the SIMD encoder — no cvtColor copy. Per-thread compressor
// is reused via thread_local to avoid TJ handle alloc cost (~30 us each).
#ifdef XINSP2_HAS_TURBOJPEG
inline bool encode_jpeg_turbo(const Image& img, int quality, std::vector<uint8_t>& out) {
    if (img.empty() || !img.data()) return false;
    int pixfmt;
    switch (img.channels) {
        case 1: pixfmt = TJPF_GRAY; break;
        case 3: pixfmt = TJPF_RGB;  break;
        case 4: pixfmt = TJPF_RGBA; break;
        default: return false;
    }
    int subsamp = (img.channels == 1) ? TJSAMP_GRAY : TJSAMP_420;
    thread_local tjhandle h = tjInitCompress();
    if (!h) return false;
    unsigned char* jpeg_buf = nullptr;
    unsigned long  jpeg_size = 0;
    int rc = tjCompress2(h, img.data(), img.width, /*pitch=*/0, img.height,
                         pixfmt, &jpeg_buf, &jpeg_size, subsamp, quality, 0);
    if (rc != 0) { if (jpeg_buf) tjFree(jpeg_buf); return false; }
    out.assign(jpeg_buf, jpeg_buf + jpeg_size);
    tjFree(jpeg_buf);
    return true;
}
#endif

// ---------- dispatch ----------

inline bool encode_jpeg(const Image& img, int quality, std::vector<uint8_t>& out) {
    if (img.empty()) return false;

#ifdef XINSP2_HAS_TURBOJPEG
    // Best path: SIMD JPEG with native RGB pixel format, no extra color-convert.
    if (encode_jpeg_turbo(img, quality, out)) return true;
#endif

    // Fallback for any build config with no libjpeg-turbo (OpenCV-free core).
    return encode_jpeg_stb(img, quality, out);
}

} // namespace xi
