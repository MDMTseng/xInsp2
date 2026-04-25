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

// ---- invert (255 - x), any channel count -------------------------------
inline Image invert(const Image& src) {
    if (src.empty()) return {};
    Image dst = alloc(src.width, src.height, src.channels);
    cv::Mat sm = as_mat(src), dm = as_mat_out(dst);
    cv::bitwise_not(sm, dm);
    return dst;
}

// ---- stats: mean / stddev / min / max ----------------------------------
struct StatsResult { double mean, stddev; int min_val, max_val, pixel_count; };
inline StatsResult stats(const Image& src) {
    StatsResult r{};
    if (src.empty() || src.channels != 1) return r;
    cv::Mat sm = as_mat(src);
    cv::Scalar mean, stddev;
    cv::meanStdDev(sm, mean, stddev);
    double mn = 0, mx = 0;
    cv::minMaxLoc(sm, &mn, &mx);
    r.mean        = mean[0];
    r.stddev      = stddev[0];
    r.min_val     = (int)mn;
    r.max_val     = (int)mx;
    r.pixel_count = src.width * src.height;
    return r;
}

// ---- adaptiveThreshold (Gaussian-mean variant) -------------------------
inline Image adaptiveThreshold(const Image& src, int block_radius, int C = 0) {
    if (src.empty() || src.channels != 1 || block_radius <= 0) return {};
    Image dst = alloc(src.width, src.height, 1);
    cv::Mat sm = as_mat(src), dm = as_mat_out(dst);
    int blockSize = 2 * block_radius + 1;
    cv::adaptiveThreshold(sm, dm, 255,
                           cv::ADAPTIVE_THRESH_MEAN_C, cv::THRESH_BINARY,
                           blockSize, (double)C);
    return dst;
}

// ---- canny edge ---------------------------------------------------------
inline Image canny(const Image& src, int low_thresh, int high_thresh) {
    if (src.empty() || src.channels != 1) return {};
    Image dst = alloc(src.width, src.height, 1);
    cv::Mat sm = as_mat(src), dm = as_mat_out(dst);
    cv::Canny(sm, dm, (double)low_thresh, (double)high_thresh, 3, false);
    return dst;
}

// ---- countWhiteBlobs ----------------------------------------------------
inline int countWhiteBlobs(const Image& binary) {
    if (binary.empty() || binary.channels != 1) return 0;
    cv::Mat sm = as_mat(binary), labels;
    int n = cv::connectedComponents(sm, labels, 8, CV_32S);
    return n - 1;   // label 0 is background
}

// ---- findContours ------------------------------------------------------
struct PointXY { int x, y; };
inline std::vector<std::vector<PointXY>> findContours(const Image& binary) {
    std::vector<std::vector<PointXY>> out;
    if (binary.empty() || binary.channels != 1) return out;
    cv::Mat sm = as_mat(binary);
    std::vector<std::vector<cv::Point>> cv_contours;
    cv::findContours(sm.clone(), cv_contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
    out.reserve(cv_contours.size());
    for (auto& c : cv_contours) {
        std::vector<PointXY> v;
        v.reserve(c.size());
        for (auto& p : c) v.push_back({ p.x, p.y });
        out.push_back(std::move(v));
    }
    return out;
}

// ---- matchTemplate (sum of squared differences) ------------------------
struct MatchPoint { int x, y; double score; };
inline MatchPoint matchTemplateSSD(const Image& src, const Image& templ) {
    MatchPoint r{0, 0, 1e30};
    if (src.empty() || templ.empty() ||
        src.channels != 1 || templ.channels != 1) return r;
    if (templ.width > src.width || templ.height > src.height) return r;
    cv::Mat sm = as_mat(src), tm = as_mat(templ);
    cv::Mat result;
    cv::matchTemplate(sm, tm, result, cv::TM_SQDIFF);
    double mn = 0, mx = 0; cv::Point mnLoc, mxLoc;
    cv::minMaxLoc(result, &mn, &mx, &mnLoc, &mxLoc);
    r.x = mnLoc.x; r.y = mnLoc.y; r.score = mn;
    return r;
}

} // namespace xi::ops::cv_backend
