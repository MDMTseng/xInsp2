#pragma once
//
// xi_ops.hpp — standard operator library for xInsp2 inspection scripts.
//
// Pure C++ image operations on xi::Image. No OpenCV dependency —
// these are written from scratch for portability. Each operator also
// ships an async_<name> variant via ASYNC_WRAP.
//
// Usage in a user script:
//
//   #include <xi/xi.hpp>
//   #include <xi/xi_ops.hpp>
//
//   void xi_inspect_entry(int frame) {
//       VAR(img,   cam->grab());
//       VAR(gray,  xi::ops::toGray(img));
//       VAR(blur,  xi::ops::gaussian(gray, 3));
//       VAR(edges, xi::ops::threshold(blur, 128));
//   }
//

#include "xi_async.hpp"
#include "xi_image.hpp"

#ifdef XINSP2_HAS_IPP
#  include "xi_ops_ipp.hpp"
#endif
#ifdef XINSP2_HAS_OPENCV
#  include "xi_ops_cv.hpp"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace xi::ops {

// ---- Color conversion ----

inline Image toGray(const Image& src) {
    if (src.empty()) return {};
    if (src.channels == 1) return src; // already gray
#ifdef XINSP2_HAS_IPP
    if (auto out = ipp::toGray(src); !out.empty()) return out;
#endif
#ifdef XINSP2_HAS_OPENCV
    if (auto out = cv_backend::toGray(src); !out.empty()) return out;
#endif
    Image dst(src.width, src.height, 1);
    const uint8_t* sp = src.data();
    uint8_t* dp = dst.data();
    int n = src.width * src.height;
    int ch = src.channels;
    for (int i = 0; i < n; ++i) {
        // BT.601 luminance: 0.299R + 0.587G + 0.114B
        int r = sp[i * ch + 0];
        int g = sp[i * ch + 1];
        int b = sp[i * ch + 2];
        dp[i] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
    }
    return dst;
}

// ---- Thresholding ----

inline Image threshold(const Image& src, int t, int max_val = 255) {
    if (src.empty() || src.channels != 1) return {};
#ifdef XINSP2_HAS_IPP
    if (auto out = ipp::threshold(src, t, max_val); !out.empty()) return out;
#endif
#ifdef XINSP2_HAS_OPENCV
    if (auto out = cv_backend::threshold(src, t, max_val); !out.empty()) return out;
#endif
    Image dst(src.width, src.height, 1);
    const uint8_t* sp = src.data();
    uint8_t* dp = dst.data();
    int n = src.width * src.height;
    uint8_t mv = static_cast<uint8_t>(max_val);
    for (int i = 0; i < n; ++i) {
        dp[i] = sp[i] > t ? mv : 0;
    }
    return dst;
}

// ---- Box blur (fast approximation of gaussian) ----

inline Image boxBlur(const Image& src, int radius) {
    if (src.empty() || src.channels != 1) return {};
    int w = src.width, h = src.height;
    Image dst(w, h, 1);
    const uint8_t* sp = src.data();
    uint8_t* dp = dst.data();
    int side = 2 * radius + 1;
    int area = side * side;

    // Two-pass separable box filter
    std::vector<int> tmp(w * h, 0);

    // Horizontal pass
    for (int y = 0; y < h; ++y) {
        int sum = 0;
        for (int x = -radius; x <= radius; ++x) {
            sum += sp[y * w + std::clamp(x, 0, w - 1)];
        }
        tmp[y * w + 0] = sum;
        for (int x = 1; x < w; ++x) {
            int add = sp[y * w + std::min(x + radius, w - 1)];
            int sub = sp[y * w + std::max(x - radius - 1, 0)];
            sum += add - sub;
            tmp[y * w + x] = sum;
        }
    }

    // Vertical pass
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -radius; y <= radius; ++y) {
            sum += tmp[std::clamp(y, 0, h - 1) * w + x];
        }
        dp[0 * w + x] = static_cast<uint8_t>(sum / (area));
        for (int y = 1; y < h; ++y) {
            int add = tmp[std::min(y + radius, h - 1) * w + x];
            int sub = tmp[std::max(y - radius - 1, 0) * w + x];
            sum += add - sub;
            dp[y * w + x] = static_cast<uint8_t>(sum / (area));
        }
    }

    return dst;
}

// Gaussian. With IPP: real sigma-based separable kernel. Without:
// 3-pass box blur (good enough for preview).
inline Image gaussian(const Image& src, int radius) {
#ifdef XINSP2_HAS_IPP
    if (auto out = ipp::gaussian(src, radius); !out.empty()) return out;
#endif
#ifdef XINSP2_HAS_OPENCV
    if (auto out = cv_backend::gaussian(src, radius); !out.empty()) return out;
#endif
    auto a = boxBlur(src, radius);
    auto b = boxBlur(a, radius);
    return boxBlur(b, radius);
}

// ---- Sobel edge detection ----

inline Image sobel(const Image& src) {
    if (src.empty() || src.channels != 1) return {};
#ifdef XINSP2_HAS_IPP
    if (auto out = ipp::sobel(src); !out.empty()) return out;
#endif
#ifdef XINSP2_HAS_OPENCV
    if (auto out = cv_backend::sobel(src); !out.empty()) return out;
#endif
    int w = src.width, h = src.height;
    Image dst(w, h, 1);
    const uint8_t* sp = src.data();
    uint8_t* dp = dst.data();
    std::memset(dp, 0, w * h);

    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            int gx = -sp[(y-1)*w+(x-1)] + sp[(y-1)*w+(x+1)]
                   - 2*sp[y*w+(x-1)]    + 2*sp[y*w+(x+1)]
                   - sp[(y+1)*w+(x-1)]   + sp[(y+1)*w+(x+1)];
            int gy = -sp[(y-1)*w+(x-1)] - 2*sp[(y-1)*w+x] - sp[(y-1)*w+(x+1)]
                   + sp[(y+1)*w+(x-1)]  + 2*sp[(y+1)*w+x] + sp[(y+1)*w+(x+1)];
            int mag = static_cast<int>(std::sqrt(gx * gx + gy * gy));
            dp[y * w + x] = static_cast<uint8_t>(std::min(mag, 255));
        }
    }
    return dst;
}

// ---- Invert ----

inline Image invert(const Image& src) {
    if (src.empty()) return {};
    Image dst(src.width, src.height, src.channels);
    const uint8_t* sp = src.data();
    uint8_t* dp = dst.data();
    size_t n = src.size();
    for (size_t i = 0; i < n; ++i) dp[i] = 255 - sp[i];
    return dst;
}

// ---- Morphology ----

inline Image erode(const Image& src, int radius = 1) {
    if (src.empty() || src.channels != 1) return {};
#ifdef XINSP2_HAS_IPP
    if (auto out = ipp::erode(src, radius); !out.empty()) return out;
#endif
#ifdef XINSP2_HAS_OPENCV
    if (auto out = cv_backend::erode(src, radius); !out.empty()) return out;
#endif
    int w = src.width, h = src.height;
    Image dst(w, h, 1);
    const uint8_t* sp = src.data();
    uint8_t* dp = dst.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t mn = 255;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    int yy = std::clamp(y + dy, 0, h - 1);
                    int xx = std::clamp(x + dx, 0, w - 1);
                    mn = std::min(mn, sp[yy * w + xx]);
                }
            }
            dp[y * w + x] = mn;
        }
    }
    return dst;
}

inline Image dilate(const Image& src, int radius = 1) {
    if (src.empty() || src.channels != 1) return {};
#ifdef XINSP2_HAS_IPP
    if (auto out = ipp::dilate(src, radius); !out.empty()) return out;
#endif
#ifdef XINSP2_HAS_OPENCV
    if (auto out = cv_backend::dilate(src, radius); !out.empty()) return out;
#endif
    int w = src.width, h = src.height;
    Image dst(w, h, 1);
    const uint8_t* sp = src.data();
    uint8_t* dp = dst.data();
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t mx = 0;
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    int yy = std::clamp(y + dy, 0, h - 1);
                    int xx = std::clamp(x + dx, 0, w - 1);
                    mx = std::max(mx, sp[yy * w + xx]);
                }
            }
            dp[y * w + x] = mx;
        }
    }
    return dst;
}

// ---- Statistics ----

struct ImageStats {
    double mean = 0;
    double stddev = 0;
    int min_val = 0;
    int max_val = 0;
    int pixel_count = 0;
};

inline ImageStats stats(const Image& src) {
    if (src.empty() || src.channels != 1) return {};
    const uint8_t* sp = src.data();
    int n = src.width * src.height;
    int64_t sum = 0, sum2 = 0;
    int mn = 255, mx = 0;
    for (int i = 0; i < n; ++i) {
        int v = sp[i];
        sum  += v;
        sum2 += v * v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
    }
    ImageStats s;
    s.pixel_count = n;
    s.mean = (double)sum / n;
    s.stddev = std::sqrt((double)sum2 / n - s.mean * s.mean);
    s.min_val = mn;
    s.max_val = mx;
    return s;
}

// ---- Morphology: open / close ------------------------------------------
//
// open  = erode → dilate  (removes small bright specks)
// close = dilate → erode  (fills small dark holes)
//
inline Image open(const Image& src, int radius = 1) {
    return dilate(erode(src, radius), radius);
}

inline Image close(const Image& src, int radius = 1) {
    return erode(dilate(src, radius), radius);
}

// ---- Adaptive threshold ------------------------------------------------
// Pixel stays if (src - local_mean) > C. Useful for uneven lighting.
inline Image adaptiveThreshold(const Image& src, int block_radius, int C = 0) {
    if (src.empty() || src.channels != 1) return {};
    auto mean = boxBlur(src, block_radius);
    int w = src.width, h = src.height;
    Image dst(w, h, 1);
    const uint8_t* sp = src.data();
    const uint8_t* mp = mean.data();
    uint8_t* dp = dst.data();
    int n = w * h;
    for (int i = 0; i < n; ++i) {
        int diff = (int)sp[i] - (int)mp[i] - C;
        dp[i] = diff > 0 ? 255 : 0;
    }
    return dst;
}

// ---- Canny edge detector (simplified) ----------------------------------
// Flow: Sobel gradient magnitude + direction → non-max suppression →
// double-threshold hysteresis. Works well on pre-blurred input (call
// gaussian(gray, ...) first for best results).
inline Image canny(const Image& src, int low_thresh, int high_thresh) {
    if (src.empty() || src.channels != 1) return {};
    int w = src.width, h = src.height;
    const uint8_t* sp = src.data();
    std::vector<int>   mag(w * h, 0);
    std::vector<float> ang(w * h, 0.f);

    // Gradient
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            int gx = -sp[(y-1)*w+(x-1)] + sp[(y-1)*w+(x+1)]
                   - 2*sp[y*w+(x-1)]    + 2*sp[y*w+(x+1)]
                   - sp[(y+1)*w+(x-1)]   + sp[(y+1)*w+(x+1)];
            int gy = -sp[(y-1)*w+(x-1)] - 2*sp[(y-1)*w+x] - sp[(y-1)*w+(x+1)]
                   + sp[(y+1)*w+(x-1)]  + 2*sp[(y+1)*w+x] + sp[(y+1)*w+(x+1)];
            mag[y*w + x] = (int)std::sqrt(gx*gx + gy*gy);
            ang[y*w + x] = std::atan2((float)gy, (float)gx);
        }
    }

    // Non-max suppression: keep only ridge pixels along gradient direction
    std::vector<uint8_t> nms(w * h, 0);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            float a = ang[y*w + x] * 180.f / 3.14159265f;
            if (a < 0) a += 180.f;
            int m = mag[y*w + x];
            int n1, n2;
            if (a < 22.5f || a >= 157.5f)         { n1 = mag[y*w + (x-1)];       n2 = mag[y*w + (x+1)]; }
            else if (a < 67.5f)                    { n1 = mag[(y-1)*w + (x+1)];   n2 = mag[(y+1)*w + (x-1)]; }
            else if (a < 112.5f)                   { n1 = mag[(y-1)*w + x];       n2 = mag[(y+1)*w + x]; }
            else                                    { n1 = mag[(y-1)*w + (x-1)];   n2 = mag[(y+1)*w + (x+1)]; }
            if (m >= n1 && m >= n2) nms[y*w + x] = (uint8_t)std::min(m, 255);
        }
    }

    // Hysteresis: strong edges seed; weak edges kept only if connected
    Image dst(w, h, 1);
    uint8_t* dp = dst.data();
    std::memset(dp, 0, w * h);
    std::vector<std::pair<int,int>> stack;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (nms[y*w + x] >= high_thresh && dp[y*w + x] == 0) {
                stack.push_back({x, y});
                while (!stack.empty()) {
                    auto [cx, cy] = stack.back(); stack.pop_back();
                    if (cx < 0 || cx >= w || cy < 0 || cy >= h) continue;
                    if (dp[cy*w + cx]) continue;
                    if (nms[cy*w + cx] >= low_thresh) {
                        dp[cy*w + cx] = 255;
                        stack.push_back({cx-1, cy});   stack.push_back({cx+1, cy});
                        stack.push_back({cx, cy-1});   stack.push_back({cx, cy+1});
                        stack.push_back({cx-1, cy-1}); stack.push_back({cx+1, cy+1});
                        stack.push_back({cx-1, cy+1}); stack.push_back({cx+1, cy-1});
                    }
                }
            }
        }
    }
    return dst;
}

// ---- Geometry primitives ----

struct Point { int x, y; };
struct Bbox  { int x, y, w, h; };

// Axis-aligned bounding box of a point set. Empty input → {0,0,0,0}.
inline Bbox bbox(const std::vector<Point>& pts) {
    if (pts.empty()) return {0, 0, 0, 0};
    int x0 = pts[0].x, x1 = pts[0].x, y0 = pts[0].y, y1 = pts[0].y;
    for (auto& p : pts) {
        if (p.x < x0) x0 = p.x;   if (p.x > x1) x1 = p.x;
        if (p.y < y0) y0 = p.y;   if (p.y > y1) y1 = p.y;
    }
    return { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}

// ---- Contours: flood-fill extraction ----
//
// Returns one point list per connected white component in the binary
// image. Points are every pixel of the component (not just the outline
// — good enough for bbox / area; a true boundary walk is a future
// refinement). 4-connectivity.
inline std::vector<std::vector<Point>> findContours(const Image& binary) {
    std::vector<std::vector<Point>> out;
    if (binary.empty() || binary.channels != 1) return out;
    int w = binary.width, h = binary.height;
    const uint8_t* sp = binary.data();
    std::vector<uint8_t> seen(w * h, 0);
    std::vector<std::pair<int,int>> stack;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (sp[y*w + x] == 0 || seen[y*w + x]) continue;
            std::vector<Point> comp;
            stack.push_back({x, y});
            while (!stack.empty()) {
                auto [cx, cy] = stack.back(); stack.pop_back();
                if (cx < 0 || cx >= w || cy < 0 || cy >= h) continue;
                if (sp[cy*w + cx] == 0 || seen[cy*w + cx]) continue;
                seen[cy*w + cx] = 1;
                comp.push_back({cx, cy});
                stack.push_back({cx-1, cy});   stack.push_back({cx+1, cy});
                stack.push_back({cx, cy-1});   stack.push_back({cx, cy+1});
            }
            if (!comp.empty()) out.push_back(std::move(comp));
        }
    }
    return out;
}

// ---- Template matching (SSD) ----
//
// Slides `templ` across `src`, computing sum-of-squared-differences at
// each position. Returns { best x, best y, best SSD }. Lower SSD = better.
// O(W·H·TW·TH) — for small templates only.
struct MatchResult { int x, y; double score; };
inline MatchResult matchTemplateSSD(const Image& src, const Image& templ) {
    MatchResult r{ 0, 0, 1e30 };
    if (src.empty() || templ.empty() ||
        src.channels != 1 || templ.channels != 1) return r;
    if (templ.width > src.width || templ.height > src.height) return r;
    const uint8_t* sp = src.data();
    const uint8_t* tp = templ.data();
    int sw = src.width, sh = src.height;
    int tw = templ.width, th = templ.height;
    for (int y = 0; y <= sh - th; ++y) {
        for (int x = 0; x <= sw - tw; ++x) {
            double acc = 0;
            for (int ty = 0; ty < th; ++ty) {
                const uint8_t* srow = sp + (y + ty) * sw + x;
                const uint8_t* trow = tp + ty * tw;
                for (int tx = 0; tx < tw; ++tx) {
                    int d = (int)srow[tx] - (int)trow[tx];
                    acc += d * d;
                }
            }
            if (acc < r.score) { r.score = acc; r.x = x; r.y = y; }
        }
    }
    return r;
}

// ---- Connected component labeling (simple) ----

inline int countWhiteBlobs(const Image& binary) {
    if (binary.empty() || binary.channels != 1) return 0;
    int w = binary.width, h = binary.height;
    std::vector<int> labels(w * h, 0);
    const uint8_t* sp = binary.data();
    int label = 0;

    // Two-pass with union-find would be proper, but for small inspection
    // images a flood-fill is fine.
    std::vector<std::pair<int,int>> stack;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (sp[y * w + x] == 0 || labels[y * w + x] != 0) continue;
            ++label;
            stack.push_back({x, y});
            while (!stack.empty()) {
                auto [cx, cy] = stack.back();
                stack.pop_back();
                if (cx < 0 || cx >= w || cy < 0 || cy >= h) continue;
                if (sp[cy * w + cx] == 0 || labels[cy * w + cx] != 0) continue;
                labels[cy * w + cx] = label;
                stack.push_back({cx-1, cy});
                stack.push_back({cx+1, cy});
                stack.push_back({cx, cy-1});
                stack.push_back({cx, cy+1});
            }
        }
    }
    return label;
}

// Async-wrapped versions: async_toGray, async_threshold, etc.
// These live in the same namespace so user scripts can call them unqualified
// after `using namespace xi::ops;`.
template<class... A> auto async_toGray(A&&... a)          { return xi::async(toGray, std::forward<A>(a)...); }
template<class... A> auto async_threshold(A&&... a)       { return xi::async(threshold, std::forward<A>(a)...); }
template<class... A> auto async_boxBlur(A&&... a)         { return xi::async(boxBlur, std::forward<A>(a)...); }
template<class... A> auto async_gaussian(A&&... a)        { return xi::async(gaussian, std::forward<A>(a)...); }
template<class... A> auto async_sobel(A&&... a)           { return xi::async(sobel, std::forward<A>(a)...); }
template<class... A> auto async_invert(A&&... a)          { return xi::async(invert, std::forward<A>(a)...); }
template<class... A> auto async_erode(A&&... a)           { return xi::async(erode, std::forward<A>(a)...); }
template<class... A> auto async_dilate(A&&... a)          { return xi::async(dilate, std::forward<A>(a)...); }
template<class... A> auto async_open(A&&... a)            { return xi::async((Image(*)(const Image&, int))open, std::forward<A>(a)...); }
template<class... A> auto async_close(A&&... a)           { return xi::async((Image(*)(const Image&, int))close, std::forward<A>(a)...); }
template<class... A> auto async_adaptiveThreshold(A&&... a) { return xi::async(adaptiveThreshold, std::forward<A>(a)...); }
template<class... A> auto async_canny(A&&... a)           { return xi::async(canny, std::forward<A>(a)...); }
template<class... A> auto async_findContours(A&&... a)    { return xi::async(findContours, std::forward<A>(a)...); }
template<class... A> auto async_matchTemplateSSD(A&&... a){ return xi::async(matchTemplateSSD, std::forward<A>(a)...); }
template<class... A> auto async_stats(A&&... a)           { return xi::async(stats, std::forward<A>(a)...); }
template<class... A> auto async_countWhiteBlobs(A&&... a) { return xi::async(countWhiteBlobs, std::forward<A>(a)...); }

} // namespace xi::ops
