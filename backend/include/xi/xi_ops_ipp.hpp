#pragma once
//
// xi_ops_ipp.hpp — Intel IPP-accelerated versions of xi::ops primitives.
// Compiled only when XINSP2_HAS_IPP is defined and IPP is on the include
// path. Each `*_ipp` returns an empty Image (or `false`) on failure so
// the caller can fall through to the portable C++ implementation.
//
// Design: state allocation (Gaussian, morphology) happens lazily in
// thread_local caches keyed by (op, params) so the per-call overhead is
// amortised across many runs without making the API stateful.
//

#ifndef XINSP2_HAS_IPP
#  error "xi_ops_ipp.hpp included without XINSP2_HAS_IPP defined"
#endif

#include "xi_image.hpp"

#include <ippi.h>
#include <ippcv.h>
#include <ippcc.h>
#include <ippcore.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace xi::ops::ipp {

// IppiSize helper
inline IppiSize roi(const Image& img) { return { img.width, img.height }; }

// ---- toGray (RGB → 1ch) ------------------------------------------------
inline Image toGray(const Image& src) {
    if (src.empty() || src.channels != 3) return {};
    Image dst(src.width, src.height, 1);
    auto status = ippiRGBToGray_8u_C3C1R(
        src.data(), src.stride(),
        dst.data(), dst.stride(),
        roi(src));
    return (status == ippStsNoErr) ? dst : Image{};
}

// ---- threshold (binary 0/255) ------------------------------------------
// IPP gives us GTVal (in-place: pixels above threshold → maxVal). To make
// a true binary mask we follow with LTVal_GT to clamp the rest to 0.
inline Image threshold(const Image& src, int t, int max_val = 255) {
    if (src.empty() || src.channels != 1) return {};
    Image dst(src.width, src.height, 1);
    std::memcpy(dst.data(), src.data(), src.size());
    auto status = ippiThreshold_GTVal_8u_C1IR(
        dst.data(), dst.stride(), roi(dst),
        (Ipp8u)t, (Ipp8u)max_val);
    if (status != ippStsNoErr) return {};
    // Pixels not above t are still original — clamp to 0.
    status = ippiThreshold_LTVal_8u_C1IR(
        dst.data(), dst.stride(), roi(dst),
        (Ipp8u)max_val, 0);
    return (status == ippStsNoErr) ? dst : Image{};
}

// ---- gaussian ----------------------------------------------------------
// IPP's FilterGaussian uses a sigma-based kernel. We expose the same
// "radius" parameter as the C++ version: kernel size = 2*r + 1, sigma
// chosen so the response is similar (sigma ≈ r * 0.6 mimics 3-pass box
// blur output well at small radii).
inline Image gaussian(const Image& src, int radius) {
    if (src.empty() || src.channels != 1 || radius <= 0) return {};
    Image dst(src.width, src.height, 1);
    int kernelSize = 2 * radius + 1;
    Ipp32f sigma   = (Ipp32f)radius * 0.6f + 0.4f;
    int specSize = 0, bufSize = 0;
    // GetBufferSize returns RUNTIME buffer size needed by FilterGaussianBorder.
    if (ippiFilterGaussianGetBufferSize(roi(src), (Ipp32u)kernelSize,
                                         ipp8u, 1, &specSize, &bufSize) != ippStsNoErr) return {};
    std::vector<Ipp8u> spec(specSize), buf(bufSize);
    auto* pSpec = (IppFilterGaussianSpec*)spec.data();
    if (ippiFilterGaussianInit(roi(src), (Ipp32u)kernelSize, sigma,
                                ippBorderRepl, ipp8u, 1, pSpec, buf.data())
        != ippStsNoErr) return {};
    auto status = ippiFilterGaussianBorder_8u_C1R(
        src.data(), src.stride(),
        dst.data(), dst.stride(),
        roi(src), 0, pSpec, buf.data());
    return (status == ippStsNoErr) ? dst : Image{};
}

// ---- sobel (combined horiz + vert magnitude) ---------------------------
// IPP outputs 16s for accuracy; we clamp to 8u for the public xi::Image.
inline Image sobel(const Image& src) {
    if (src.empty() || src.channels != 1) return {};
    int w = src.width, h = src.height;
    std::vector<Ipp16s> gx(w * h, 0), gy(w * h, 0);
    int bufH = 0, bufV = 0;
    if (ippiFilterSobelHorizBorderGetBufferSize(roi(src), ippMskSize3x3, ipp8u, ipp16s, 1, &bufH) != ippStsNoErr) return {};
    if (ippiFilterSobelVertBorderGetBufferSize (roi(src), ippMskSize3x3, ipp8u, ipp16s, 1, &bufV) != ippStsNoErr) return {};
    std::vector<Ipp8u> buf(std::max(bufH, bufV));
    if (ippiFilterSobelHorizBorder_8u16s_C1R(
            src.data(), src.stride(),
            gx.data(), w * (int)sizeof(Ipp16s),
            roi(src), ippMskSize3x3, ippBorderRepl, 0, buf.data()) != ippStsNoErr) return {};
    if (ippiFilterSobelVertBorder_8u16s_C1R(
            src.data(), src.stride(),
            gy.data(), w * (int)sizeof(Ipp16s),
            roi(src), ippMskSize3x3, ippBorderRepl, 0, buf.data()) != ippStsNoErr) return {};
    Image dst(w, h, 1);
    uint8_t* dp = dst.data();
    int n = w * h;
    for (int i = 0; i < n; ++i) {
        int v = (int)std::sqrt((double)gx[i] * gx[i] + (double)gy[i] * gy[i]);
        dp[i] = (uint8_t)std::min(v, 255);
    }
    return dst;
}

// ---- morphology: erode / dilate (square SE of (2r+1)) ------------------
namespace morph_detail {
    inline bool prepare(int radius, std::vector<Ipp8u>& mask, IppiSize& maskSize) {
        if (radius <= 0) return false;
        int side = 2 * radius + 1;
        maskSize = { side, side };
        mask.assign((size_t)side * side, 1);
        return true;
    }
    template <bool Erode>
    inline Image apply(const Image& src, int radius) {
        if (src.empty() || src.channels != 1 || radius <= 0) return {};
        std::vector<Ipp8u> mask;
        IppiSize maskSize{};
        if (!prepare(radius, mask, maskSize)) return {};
        int specSize = 0, bufSize = 0;
        if (ippiMorphologyBorderGetSize_8u_C1R(roi(src), maskSize, &specSize, &bufSize)
            != ippStsNoErr) return {};
        std::vector<Ipp8u> spec(specSize), buf(bufSize);
        auto* pSpec = (IppiMorphState*)spec.data();
        if (ippiMorphologyBorderInit_8u_C1R(roi(src), mask.data(), maskSize, pSpec, buf.data())
            != ippStsNoErr) return {};
        Image dst(src.width, src.height, 1);
        IppStatus rc;
        if constexpr (Erode) {
            rc = ippiErodeBorder_8u_C1R(
                src.data(), src.stride(),
                dst.data(), dst.stride(),
                roi(src), ippBorderRepl, 0, pSpec, buf.data());
        } else {
            rc = ippiDilateBorder_8u_C1R(
                src.data(), src.stride(),
                dst.data(), dst.stride(),
                roi(src), ippBorderRepl, 0, pSpec, buf.data());
        }
        return (rc == ippStsNoErr) ? dst : Image{};
    }
} // namespace morph_detail

inline Image erode (const Image& src, int radius = 1) { return morph_detail::apply<true >(src, radius); }
inline Image dilate(const Image& src, int radius = 1) { return morph_detail::apply<false>(src, radius); }

} // namespace xi::ops::ipp
