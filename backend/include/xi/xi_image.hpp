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
    // A freshly-allocated buffer is WRITABLE (this Image is its sole owner), so
    // write() is the blessed sink accessor on it.
    Image(int w, int h, int c)
        : width(w > 0 ? w : 0), height(h > 0 ? h : 0), channels(c > 0 ? c : 0) {
        if (width > 0 && height > 0 && channels > 0) {
            auto vec = std::make_shared<std::vector<uint8_t>>(
                static_cast<size_t>(width) * height * channels);
            // Aliasing ctor: owns `vec`, exposes its first byte.
            pixels_ = std::shared_ptr<uint8_t>(vec, vec->data());
            pixels_size_ = vec->size();
            writable_ = true;
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
            writable_ = true;   // fresh owned copy — this Image is its sole owner
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
        // A freshly-created output slot the plugin uniquely owns — the blessed
        // WRITABLE path (write() returns a mutable pointer). adopt_pool_handle
        // marks it read-only by default (its common caller is an INPUT view); a
        // create_in_pool result is the one pool-backed image that IS an output.
        img.writable_ = true;
        return img;
    }

    // Allocate a fresh, WRITABLE output slot in the host's ImagePool. Alias of
    // create_in_pool spelled to make INTENT explicit at the call site: this is an
    // OUTPUT you will write into (its write() yields a mutable uint8_t*), as
    // opposed to an INPUT view (adopt_pool_handle), whose blessed accessor is the
    // const read(). Prefer this + write() over data() when producing an image so
    // the read-only-input / writable-output invariant is visible in the code.
    static Image output_image(const xi_host_api* host, int w, int h, int c) {
        return create_in_pool(host, w, h, c);
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

    // --- Read/write access discipline (external review 02 I.4) --------------
    // The blessed accessors that encode the read-only-input / writable-output
    // invariant in the type system, mirroring the host's xi.imaging_rw@1 door.

    // read() — the blessed READ accessor, const on ANY image (input view OR
    // output). Use this to read pixels of an INPUT: a trigger/input image is a
    // zero-copy view aliased across consumers, so reading is the only safe verb.
    const uint8_t* read() const { return pixels_.get(); }

    // write() — the blessed WRITE accessor, VALID ONLY on a writable OUTPUT
    // (one made via output_image()/create_in_pool()/a fresh heap Image). On an
    // INPUT-origin image (adopt_pool_handle view / xi::Image::view) it returns
    // nullptr — the strict, correct behaviour: an input is read-only, and a
    // plugin that wants to mutate must allocate its OWN output and write there.
    // NO silent copy-on-write. `writable()` lets a caller test before writing.
    bool     writable() const { return writable_; }
    uint8_t* write() { return writable_ ? pixels_.get() : nullptr; }

    // data() — the LEGACY always-mutable accessor. KEPT so existing operator code
    // stays green, but it is the ESCAPE HATCH: the non-const overload on an
    // INPUT-origin image (one from adopt_pool_handle / a trigger .image("x")) is
    // WRONG — writing through it mutates pool memory ALIASED by other consumers
    // and silently corrupts their input. Prefer read() to read an input and
    // write()/output_image() to produce an output; reach for data() only when you
    // knowingly own the buffer and need the raw pointer.
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
    // Access discipline (external review 02 I.4): true only for a buffer THIS
    // Image uniquely owns / produced as an output (fresh heap ctor, owned copy,
    // output_image/create_in_pool). false for an INPUT view (adopt_pool_handle,
    // view) — write() then returns nullptr. Defaults false so any path that does
    // not explicitly opt in (an input view) is read-only.
    bool                     writable_ = false;
};

// cv::Mat -> owning xi::Image is the free function `xi::from_cv_mat(mat)`,
// defined in the opt-in header <xi/xi_cv.hpp> (kept out of here so this
// header stays OpenCV-free).

} // namespace xi
