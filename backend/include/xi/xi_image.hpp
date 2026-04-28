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
// Storage: an Image holds a `shared_ptr<uint8_t>` over its first byte.
// The buffer can be backed by either a heap vector (when ops allocate
// new output) or by a refcounted handle in the host's ImagePool / SHM
// region (zero-copy view). The two cases are indistinguishable through
// `data()` / `size()` / `stride()`, so operator code is unaffected.
// `record_to_c` / `UseProxy::process` shortcut the pool-backed case to
// addref instead of memcpy on the way across the ABI boundary, which
// is the whole reason the pool-backed branch exists.
//

#include "xi_abi.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace xi {

struct Image {
    int width    = 0;
    int height   = 0;
    int channels = 0;   // 1 (gray), 3 (RGB), or 4 (RGBA)

    Image() = default;

    // Allocate a fresh heap buffer of the given dimensions (zero-initialised).
    Image(int w, int h, int c)
        : width(w > 0 ? w : 0), height(h > 0 ? h : 0), channels(c > 0 ? c : 0) {
        if (width > 0 && height > 0 && channels > 0) {
            auto vec = std::make_shared<std::vector<uint8_t>>(
                static_cast<size_t>(width) * height * channels);
            // Aliasing ctor: owns `vec`, exposes its first byte.
            pixels_ = std::shared_ptr<uint8_t>(vec, vec->data());
            pixels_size_ = vec->size();
        }
    }

    // Copy an existing buffer into a fresh heap buffer.
    Image(int w, int h, int c, const uint8_t* data)
        : width(w > 0 ? w : 0), height(h > 0 ? h : 0), channels(c > 0 ? c : 0) {
        if (data && width > 0 && height > 0 && channels > 0) {
            auto vec = std::make_shared<std::vector<uint8_t>>(
                data, data + static_cast<size_t>(width) * height * channels);
            pixels_ = std::shared_ptr<uint8_t>(vec, vec->data());
            pixels_size_ = vec->size();
        }
    }

    // Zero-copy view over a refcounted host pool handle. Bumps refcount
    // on construction; releases on the last copy's destruction. The
    // returned Image's `data()` points directly at pool memory — no
    // bytes copied. Works for both in-process pool handles and SHM
    // handles (host_api routes addref / release correctly for both).
    static Image adopt_pool_handle(const xi_host_api* host, xi_image_handle h) {
        Image img;
        if (!host || !h) return img;
        int w  = host->image_width(h);
        int hh = host->image_height(h);
        int ch = host->image_channels(h);
        if (w <= 0 || hh <= 0 || ch <= 0) return Image{};
        host->image_addref(h);
        img.width    = w;
        img.height   = hh;
        img.channels = ch;
        img.pixels_  = std::shared_ptr<uint8_t>(
            host->image_data(h),
            [host, h](uint8_t*) { host->image_release(h); });
        img.pixels_size_ = static_cast<size_t>(w) * hh * ch;
        img.pool_host_   = host;
        img.pool_handle_ = h;
        return img;
    }

    bool   empty() const { return width == 0 || height == 0 || channels == 0; }
    size_t size()  const { return pixels_size_; }
    uint8_t*       data()       { return pixels_.get(); }
    const uint8_t* data() const { return pixels_.get(); }
    int    stride() const { return width * channels; }

    // Pool-backed introspection — non-zero only when this Image is a
    // zero-copy view over a host handle. Used by record_to_c and
    // UseProxy to skip a memcpy on the cross-ABI return path.
    const xi_host_api* pool_host()   const { return pool_host_; }
    xi_image_handle    pool_handle() const { return pool_handle_; }

private:
    std::shared_ptr<uint8_t> pixels_;
    size_t                   pixels_size_ = 0;
    const xi_host_api*       pool_host_   = nullptr;
    xi_image_handle          pool_handle_ = XI_IMAGE_NULL;
};

} // namespace xi
