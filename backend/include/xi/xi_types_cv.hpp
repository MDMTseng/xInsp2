#pragma once
//
// xi_types_cv.hpp — OpenCV interop for the small nominal vector types.
//
// Opt-in: this is the ONLY xi header that pulls <opencv2/core.hpp>, so
// xi_types.hpp itself stays dependency-free. Include this when you want to drop
// a Vec2/3/4 (or Point) into cv:: matrix/vector math and convert the result
// back:
//
//   xi::Vec3   v  = ...;
//   cv::Vec3d  r  = M33 * xi::to_cv(v);     // cv math (Matx / Mat / Vec ...)
//   xi::Vec3   v2 = xi::from_cv(r);          // back to a nominal value
//
// to_cv gives a cv::Vec (stack, fast, interops with cv::Matx); for bigger data
// build a cv::Mat from the components yourself.
//
#include "xi_types.hpp"

#include <opencv2/core.hpp>

namespace xi {

inline cv::Vec2d to_cv(const Vec2&  v) { return cv::Vec2d(v.x(), v.y()); }
inline cv::Vec3d to_cv(const Vec3&  v) { return cv::Vec3d(v.x(), v.y(), v.z()); }
inline cv::Vec4d to_cv(const Vec4&  v) { return cv::Vec4d(v.x(), v.y(), v.z(), v.w()); }
inline cv::Vec2d to_cv(const Point& p) { return cv::Vec2d(p.x(), p.y()); }

inline Vec2 from_cv(const cv::Vec2d& c) { return Vec2(c[0], c[1]); }
inline Vec3 from_cv(const cv::Vec3d& c) { return Vec3(c[0], c[1], c[2]); }
inline Vec4 from_cv(const cv::Vec4d& c) { return Vec4(c[0], c[1], c[2], c[3]); }

// N×N matrices <-> cv::Matx. Drop a MatN into cv:: math (M * v, M1 * M2, inv,
// solve, ...) and convert the result back.
template <int N>
inline cv::Matx<double, N, N> to_cv(const MatN<N>& m) {
    cv::Matx<double, N, N> out;
    for (int r = 0; r < N; ++r)
        for (int c = 0; c < N; ++c) out(r, c) = m.at(r, c);
    return out;
}
template <int N>
inline MatN<N> from_cv(const cv::Matx<double, N, N>& m) {
    MatN<N> out;
    for (int r = 0; r < N; ++r)
        for (int c = 0; c < N; ++c) out.set(r, c, m(r, c));
    return out;
}

} // namespace xi
