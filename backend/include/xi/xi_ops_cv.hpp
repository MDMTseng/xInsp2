#pragma once
//
// xi_ops_cv.hpp — OpenCV-backed implementations of xi::ops primitives.
// Compiled only when XINSP2_HAS_OPENCV is defined and OpenCV is on the
// include path. Each `*_cv` returns an empty Image on failure so the
// caller can fall through to the next backend.
//
// We construct cv::Mat as a non-owning view over xi::Image's pixel
// buffer (zero-copy) and write the output directly into a new
// xi::Image's buffer (also zero-copy at the cv::Mat boundary).
//

#ifndef XINSP2_HAS_OPENCV
#  error "xi_ops_cv.hpp included without XINSP2_HAS_OPENCV defined"
#endif

#include "xi_image.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace xi::ops::cv_backend {

// Map xi::Image → cv::Mat (no copy)
inline cv::Mat as_mat(const Image& img) {
    int t = (img.channels == 1) ? CV_8UC1
          : (img.channels == 3) ? CV_8UC3
          : (img.channels == 4) ? CV_8UC4
          : -1;
    if (t < 0) return {};
    return cv::Mat(img.height, img.width, t,
                   const_cast<uint8_t*>(img.data()), img.stride());
}

// Allocate xi::Image and wrap as cv::Mat for output.
inline Image alloc(int w, int h, int c) { return Image(w, h, c); }
inline cv::Mat as_mat_out(Image& dst) {
    return cv::Mat(dst.height, dst.width,
                   dst.channels == 1 ? CV_8UC1 : (dst.channels == 3 ? CV_8UC3 : CV_8UC4),
                   dst.data(), dst.stride());
}

// ---- toGray ------------------------------------------------------------
inline Image toGray(const Image& src) {
    if (src.empty() || src.channels != 3) return {};
    Image dst = alloc(src.width, src.height, 1);
    cv::Mat sm = as_mat(src), dm = as_mat_out(dst);
    cv::cvtColor(sm, dm, cv::COLOR_RGB2GRAY);
    return dst;
}

// ---- threshold (binary 0/255) ------------------------------------------
inline Image threshold(const Image& src, int t, int max_val = 255) {
    if (src.empty() || src.channels != 1) return {};
    Image dst = alloc(src.width, src.height, 1);
    cv::Mat sm = as_mat(src), dm = as_mat_out(dst);
    cv::threshold(sm, dm, (double)t, (double)max_val, cv::THRESH_BINARY);
    return dst;
}

// ---- gaussian ----------------------------------------------------------
inline Image gaussian(const Image& src, int radius) {
    if (src.empty() || src.channels != 1 || radius <= 0) return {};
    int k = 2 * radius + 1;                  // odd kernel size
    double sigma = (double)radius * 0.6 + 0.4;
    Image dst = alloc(src.width, src.height, 1);
    cv::Mat sm = as_mat(src), dm = as_mat_out(dst);
    cv::GaussianBlur(sm, dm, cv::Size(k, k), sigma, sigma, cv::BORDER_REPLICATE);
    return dst;
}

// ---- sobel (combined magnitude, clamped to 8u) -------------------------
inline Image sobel(const Image& src) {
    if (src.empty() || src.channels != 1) return {};
    cv::Mat sm = as_mat(src);
    cv::Mat gx, gy;
    cv::Sobel(sm, gx, CV_16S, 1, 0, 3, 1.0, 0.0, cv::BORDER_REPLICATE);
    cv::Sobel(sm, gy, CV_16S, 0, 1, 3, 1.0, 0.0, cv::BORDER_REPLICATE);
    cv::Mat mag;
    cv::magnitude(cv::Mat_<float>(gx), cv::Mat_<float>(gy), mag);
    Image dst = alloc(src.width, src.height, 1);
    cv::Mat dm = as_mat_out(dst);
    mag.convertTo(dm, CV_8U);
    return dst;
}

// ---- morphology (square SE of (2r+1)) ----------------------------------
inline Image erode(const Image& src, int radius = 1) {
    if (src.empty() || src.channels != 1 || radius <= 0) return {};
    Image dst = alloc(src.width, src.height, 1);
    cv::Mat se = cv::getStructuringElement(cv::MORPH_RECT,
                                           cv::Size(2*radius+1, 2*radius+1));
    cv::Mat sm = as_mat(src), dm = as_mat_out(dst);
    cv::erode(sm, dm, se, cv::Point(-1,-1), 1, cv::BORDER_REPLICATE);
    return dst;
}

inline Image dilate(const Image& src, int radius = 1) {
    if (src.empty() || src.channels != 1 || radius <= 0) return {};
    Image dst = alloc(src.width, src.height, 1);
    cv::Mat se = cv::getStructuringElement(cv::MORPH_RECT,
                                           cv::Size(2*radius+1, 2*radius+1));
    cv::Mat sm = as_mat(src), dm = as_mat_out(dst);
    cv::dilate(sm, dm, se, cv::Point(-1,-1), 1, cv::BORDER_REPLICATE);
    return dst;
}

} // namespace xi::ops::cv_backend
