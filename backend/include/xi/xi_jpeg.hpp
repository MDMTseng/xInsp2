#pragma once
//
// xi_jpeg.hpp — encode xi::Image to JPEG bytes.
//
// Three backends, selected at runtime by CPU vendor:
//   - Intel CPU + XINSP2_HAS_IPP:  Intel IPP (fastest on Intel)
//   - Non-Intel + XINSP2_HAS_OPENCV: OpenCV imencode (good AVX2 via turbo)
//   - Fallback: stb_image_write (no deps, slowest)
//
// Build flags:
//   -DXINSP2_HAS_IPP=1      — link against IPP libs
//   -DXINSP2_HAS_OPENCV=1   — link against OpenCV
//   (neither)                — stb fallback only
//

#include <cstdint>
#include <vector>

#include "xi_image.hpp"

#ifdef XINSP2_HAS_IPP
  #include <ippi.h>
  #include <ippj.h>
#endif

#ifdef XINSP2_HAS_OPENCV
  #include <opencv2/imgcodecs.hpp>
  #include <opencv2/core/mat.hpp>
#endif

// stb forward-declare (always available as fallback)
extern "C" int stbi_write_jpg_to_func(
    void (*func)(void* context, void* data, int size),
    void* context,
    int x, int y, int comp, const void* data, int quality);

namespace xi {

// ---------- CPU vendor detection via CPUID ----------

enum class CpuVendor { Intel, AMD, Other };

inline CpuVendor detect_cpu_vendor() {
    static CpuVendor cached = [] {
#if defined(_MSC_VER)
        int info[4];
        __cpuid(info, 0);
        char vendor[13] = {};
        *reinterpret_cast<int*>(vendor + 0) = info[1]; // EBX
        *reinterpret_cast<int*>(vendor + 4) = info[3]; // EDX
        *reinterpret_cast<int*>(vendor + 8) = info[2]; // ECX
#elif defined(__GNUC__) || defined(__clang__)
        unsigned int eax, ebx, ecx, edx;
        __cpuid(0, eax, ebx, ecx, edx);
        char vendor[13] = {};
        *reinterpret_cast<unsigned int*>(vendor + 0) = ebx;
        *reinterpret_cast<unsigned int*>(vendor + 4) = edx;
        *reinterpret_cast<unsigned int*>(vendor + 8) = ecx;
#else
        char vendor[13] = "Unknown";
#endif
        if (std::string_view(vendor, 12) == "GenuineIntel") return CpuVendor::Intel;
        if (std::string_view(vendor, 12) == "AuthenticAMD") return CpuVendor::AMD;
        return CpuVendor::Other;
    }();
    return cached;
}

// ---------- IPP backend ----------

#ifdef XINSP2_HAS_IPP
inline bool encode_jpeg_ipp(const Image& img, int quality, std::vector<uint8_t>& out) {
    // IPP JPEG encode via ippiEncodeJPEG.
    // The user provides their own IPP integration here — IPP's JPEG API
    // requires several setup steps (quantization tables, Huffman tables,
    // state allocation) that depend on the specific IPP version installed.
    //
    // Skeleton:
    //   IppiEncodeState* state;
    //   ippiEncodeJPEGGetBufSize(..., &bufSize);
    //   out.resize(bufSize);
    //   ippiEncodeJPEG_8u_C3(..., out.data(), &encodedSize, state);
    //   out.resize(encodedSize);
    //
    // For now, this is a placeholder — fill in with your IPP setup.
    // Return false to fall through to the next backend.
    (void)img; (void)quality; (void)out;
    return false;
}
#endif

// ---------- OpenCV backend ----------

#ifdef XINSP2_HAS_OPENCV
inline bool encode_jpeg_opencv(const Image& img, int quality, std::vector<uint8_t>& out) {
    if (img.empty() || !img.data()) return false;

    int cv_type = 0;
    switch (img.channels) {
        case 1: cv_type = CV_8UC1; break;
        case 3: cv_type = CV_8UC3; break;
        case 4: cv_type = CV_8UC4; break;
        default: return false;
    }

    // Wrap without copy — xi::Image is row-major interleaved, same as cv::Mat.
    cv::Mat mat(img.height, img.width, cv_type,
                const_cast<uint8_t*>(img.data()), img.stride());

    // OpenCV imencode expects BGR for 3-channel; xi::Image is RGB.
    cv::Mat bgr;
    if (img.channels == 3) {
        cv::cvtColor(mat, bgr, cv::COLOR_RGB2BGR);
    } else {
        bgr = mat;
    }

    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality};
    return cv::imencode(".jpg", bgr, out, params);
}
#endif

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

// ---------- dispatch ----------

inline bool encode_jpeg(const Image& img, int quality, std::vector<uint8_t>& out) {
    if (img.empty()) return false;

    auto vendor = detect_cpu_vendor();

#ifdef XINSP2_HAS_IPP
    if (vendor == CpuVendor::Intel) {
        if (encode_jpeg_ipp(img, quality, out)) return true;
        // IPP failed or not implemented — fall through
    }
#endif

#ifdef XINSP2_HAS_OPENCV
    if (vendor != CpuVendor::Intel) {
        // Non-Intel: prefer OpenCV (libjpeg-turbo with AVX2)
        if (encode_jpeg_opencv(img, quality, out)) return true;
    }
#endif

    // Fallback for any CPU / any build config
    return encode_jpeg_stb(img, quality, out);
}

} // namespace xi
