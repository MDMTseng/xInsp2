#pragma once
//
// xi_image.hpp — the Image type for xInsp2 inspection routines.
//
// A minimal, value-semantic image container used by operators and by
// VAR() tracking. Deliberately NOT a cv::Mat wrapper — xi::Image is
// dependency-free (NO OpenCV) so the mandatory umbrella (xi.hpp) stays
// CV-free. Operators that want cv::Mat opt in via `#include <xi/xi_cv.hpp>`
// and borrow a non-owning view with the free function `xi::as_cv_mat(img)`
// (and copy back with `xi::from_cv_mat(mat)`).
//
// Layout is row-major, interleaved channels, uint8 pixels. That covers
// ~99% of machine-vision inspection needs. Floating-point and multi-plane
// images can be added later as alternate kinds without breaking existing
// scripts.
//
// Storage: an Image holds a `shared_ptr<uint8_t>` over its first byte.
// The buffer is backed by either a heap vector (when ops allocate new
// output) or by a refcounted handle in the host's in-process ImagePool
// (zero-copy view). The two cases are indistinguishable through
// `data()` / `size()` / `stride()`, so operator code is unaffected.
// `record_to_c` / `UseProxy::process` shortcut the pool-backed case to
// addref instead of memcpy on the way across the ABI boundary, which
// is the whole reason the pool-backed branch exists.
//

#include "xi_abi.h"

#include <atomic>
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

    // Allocate a fresh slot in the host's in-process ImagePool and return
    // a pool-backed Image whose `data()` points directly at that slot.
    // Plugins use this when they need to *produce* a new image — the
    // bytes get written straight into the pool, so the cross-ABI return
    // path (record_to_c) can short-circuit to addref instead of doing
    // a heap-to-pool memcpy.
    //
    // Allocates from the host's ImagePool via `host->image_create`. The
    // handle is usable directly by the backend and any other in-process
    // plugin or script.
    //
    // Refcount accounting: image_create returns refcount=1;
    // adopt_pool_handle adds one (=2); we release once (=1), owned by
    // the returned Image's shared_ptr deleter.
    static Image create_in_pool(const xi_host_api* host, int w, int h, int c) {
        if (!host || w <= 0 || h <= 0 || c <= 0) return Image{};
        xi_image_handle hndl = host->image_create(w, h, c);
        if (!hndl) return Image{};
        Image img = adopt_pool_handle(host, hndl);
        host->image_release(hndl);
        return img;
    }

    // Zero-copy view over a refcounted in-process host pool handle. Bumps
    // refcount on construction; releases on the last copy's destruction.
    // The returned Image's `data()` points directly at pool memory — no
    // bytes copied.
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

    // Non-owning VIEW over an external buffer (no copy, no ownership). The caller
    // guarantees `data` outlives this Image and every copy. Use on read-only paths
    // (e.g. JPEG-encoding bytes already sitting in a reused scratch vector) to skip
    // the deep copy the `(w,h,c,data)` ctor does.
    static Image view(int w, int h, int c, const uint8_t* data) {
        Image img;
        if (!data || w <= 0 || h <= 0 || c <= 0) return img;
        img.width = w; img.height = h; img.channels = c;
        img.pixels_ = std::shared_ptr<uint8_t>(const_cast<uint8_t*>(data),
                                                [](uint8_t*) {});   // no-op deleter
        img.pixels_size_ = static_cast<size_t>(w) * h * c;
        return img;
    }

    bool   empty() const { return width == 0 || height == 0 || channels == 0; }
    size_t size()  const { return pixels_size_; }
    uint8_t*       data()       { return pixels_.get(); }
    const uint8_t* data() const { return pixels_.get(); }
    int    stride() const { return width * channels; }

    // A cv::Mat view over these bytes is available as the free function
    // `xi::as_cv_mat(img)` in the opt-in header <xi/xi_cv.hpp> — kept out
    // of this header so xi::Image (and the xi.hpp umbrella) needs no OpenCV.

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

// cv::Mat -> owning xi::Image is the free function `xi::from_cv_mat(mat)`,
// defined in the opt-in header <xi/xi_cv.hpp> (kept out of here so this
// header stays OpenCV-free).

} // namespace xi
