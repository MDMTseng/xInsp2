#pragma once
//
// xi_image.hpp — the Image type for xInsp2 inspection routines.
//
// A minimal, value-semantic image container used by operators and by
// VAR() tracking. Deliberately NOT a cv::Mat wrapper — xi::Image is
// dependency-free so the core headers stay self-contained. Operators that
// want cv::Mat can borrow a non-owning view via `Image::as_cv_mat()` once
// OpenCV is pulled in by a specific op library.
//
// Layout is row-major, interleaved channels, uint8 pixels. That covers
// ~99% of machine-vision inspection needs. Floating-point and multi-plane
// images can be added later as alternate kinds without breaking existing
// scripts.
//

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace xi {

struct Image {
    int      width    = 0;
    int      height   = 0;
    int      channels = 0;   // 1 (gray), 3 (RGB), or 4 (RGBA)
    std::shared_ptr<std::vector<uint8_t>> pixels;

    Image() = default;

    Image(int w, int h, int c)
        : width(w), height(h), channels(c),
          pixels(std::make_shared<std::vector<uint8_t>>(
              static_cast<size_t>(w) * h * c)) {}

    // Construct from an existing buffer (copied).
    Image(int w, int h, int c, const uint8_t* data)
        : width(w), height(h), channels(c),
          pixels(std::make_shared<std::vector<uint8_t>>(
              data, data + static_cast<size_t>(w) * h * c)) {}

    bool   empty() const { return width == 0 || height == 0 || channels == 0; }
    size_t size()  const { return pixels ? pixels->size() : 0; }
    uint8_t*       data()       { return pixels ? pixels->data() : nullptr; }
    const uint8_t* data() const { return pixels ? pixels->data() : nullptr; }

    int stride() const { return width * channels; }
};

} // namespace xi
