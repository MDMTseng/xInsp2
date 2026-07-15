#pragma once
//
// xi_cv.hpp — OpenCV convenience helpers with xInsp2's RGB convention baked in.
//
// xi::Image stores pixels RGB-ordered; OpenCV defaults to BGR. These wrap the
// conversions so each plugin doesn't re-derive them (was DM-12: every plugin
// hand-rolled image<->Mat copies and re-remembered the RGB->BGR-before-encode
// rule). For the zero-copy path use the free `xi::as_cv_mat(img)` (non-owning
// view) and Plugin::pool_image() directly — see docs/guides/write-a-plugin.md.
//
// This is the ONE opt-in header where xi::Image meets OpenCV: including it is
// what pulls <opencv2/*> into a plugin/script. The mandatory umbrella xi.hpp
// (and xi_image.hpp) is deliberately OpenCV-free — a no-CV routine that only
// does #include <xi/xi.hpp> compiles with OpenCV OFF the include path.
//
#include "xi_image.hpp"
#include "xi_image_blob.hpp"   // xi::ImageBlobView + read_image_blob (CV-free)

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace xi {

// Non-owning cv::Mat VIEW over an Image's bytes (no allocation, no copy). This
// is the zero-copy bridge that used to be Image::as_cv_mat(); it now lives here
// as a free function so xi::Image itself needs no OpenCV. Plugin code typically:
//
//   auto src = input.get_image("src");
//   auto dst = xi::Image::create_in_pool(host(), w, h, 1);
//   cv::GaussianBlur(xi::as_cv_mat(src), xi::as_cv_mat(dst), {0,0}, 2.0);
//   return xi::Record().image("blurred", dst);
//
// Both Mats are non-owning — they hold pointers into pool memory owned by the
// xi::Image's shared_ptr. The Mat must not outlive the xi::Image.
inline cv::Mat as_cv_mat(const Image& img) {
    if (img.empty()) return {};
    int type = CV_8UC(img.channels);
    return cv::Mat(img.height, img.width, type,
                   const_cast<uint8_t*>(img.data()),
                   static_cast<size_t>(img.stride()));
}

// --- Read/write access-discipline views (external review 02 I.4) ------------
// OpenCV has no const cv::Mat (a Mat always carries a mutable data pointer), so
// the read-only-input / writable-output invariant is encoded in the FUNCTION
// SURFACE instead: as_cv_read yields a view meant for READING an input, and
// as_cv_write a view for WRITING an output. Shortest correct plugin body:
//
//   const xi::Image src = in.get_image("frame");            // input view
//   xi::Image       dst = pool_image(src.width, src.height, 1);   // output
//   cv::threshold(xi::as_cv_read(src), xi::as_cv_write(dst), 128, 255, cv::THRESH_BINARY);
//   return xi::Record().image("binary", dst);

// READ view over an INPUT (or any) image — a cv::Mat over the pixels reached via
// Image::read(). By contract callers pass it only as a cv:: SOURCE argument; the
// const Image& parameter is the signal "I am reading this, not writing it".
inline cv::Mat as_cv_read(const Image& img) {
    if (img.empty()) return {};
    int type = CV_8UC(img.channels);
    return cv::Mat(img.height, img.width, type,
                   const_cast<uint8_t*>(img.read()),
                   static_cast<size_t>(img.stride()));
}

// WRITE view over a WRITABLE OUTPUT image — a cv::Mat over the pixels reached via
// Image::write(). Returns an EMPTY Mat if `img` is not writable (an input view):
// writing into an empty Mat is a loud OpenCV error rather than silent corruption
// of an aliased input, matching the host's image_write returning null for a
// shared handle. Use only on an output_image()/pool_image() result.
inline cv::Mat as_cv_write(Image& img) {
    uint8_t* p = img.write();
    if (!p || img.empty()) return {};
    int type = CV_8UC(img.channels);
    return cv::Mat(img.height, img.width, type, p,
                   static_cast<size_t>(img.stride()));
}

// Ergonomic short aliases used in docs / templates: as_cv(src_read) reads, and
// as_cv(dst_write) writes. Overload resolution picks the write path only for a
// non-const (writable) Image lvalue; a const/ input Image binds the read path.
inline cv::Mat as_cv(const Image& img) { return as_cv_read(img); }
inline cv::Mat as_cv(Image& img) {
    // A writable output routes to the write view; an input-origin Image (not
    // writable) falls back to the read view so as_cv(src) on a non-const local
    // input still reads correctly.
    return img.writable() ? as_cv_write(img) : as_cv_read(img);
}

// Copy a cv::Mat into an OWNING xi::Image — the inverse of xi::as_cv_mat().
// Use this to record / return an intermediate cv::Mat (e.g. a threshold mask or
// a background-subtraction response) without worrying about the Mat outliving
// the Image: the bytes are copied into the Image's own buffer.
//
// Supports 8-bit 1/3/4-channel mats; a non-continuous (ROI / sub-mat) is cloned
// first so rows are packed. Returns an empty Image for an empty or non-8-bit mat
// (convert depth first, e.g. `m.convertTo(tmp, CV_8U)`), so callers can check
// .empty() rather than getting a malformed image.
inline Image from_cv_mat(const cv::Mat& m) {
    if (m.empty() || m.depth() != CV_8U) return Image{};
    int c = m.channels();
    if (c != 1 && c != 3 && c != 4) return Image{};
    cv::Mat src = m.isContinuous() ? m : m.clone();
    return Image(src.cols, src.rows, c, src.data);
}

// Owning cv::Mat COPY of an Image (RGB order, like the bytes). Use this when you
// need a Mat that outlives the Image; for an in-place op prefer the zero-copy
// non-owning `xi::as_cv_mat(img)`.
inline cv::Mat to_cv(const Image& img) { return as_cv_mat(img).clone(); }

// cv::Mat -> owning xi::Image. The Mat must already be in xInsp2's RGB order
// (e.g. one produced by as_cv_mat / to_cv, NOT a raw cv::imread which is BGR).
// Naming-symmetric alias of from_cv_mat().
inline Image to_image(const cv::Mat& m) { return from_cv_mat(m); }

// --- cv::Mat sugar over an `xi/image` self-describing blob (spec 30) ----------
// The consumer counterpart to the xi::Image as_cv_* views above: wrap a Pack
// `xi/image` blob (parsed by xi::read_image_blob into an ImageBlobView — see
// <xi/xi_image_blob.hpp>, reached via PackIn::image_blob / ScriptPack::image_blob)
// as a cv::Mat, typed by the descriptor's w/h/c/dt. No blob-head parsing here —
// the descriptor already carries the shape and dtype.

// Map an `xi/image` "dt" string to an OpenCV depth constant; -1 if the dtype has
// no cv:: representation (fail-loud, never a silent reinterpret).
inline int cv_depth_for_dt(std::string_view dt) {
    if (dt == "u8")  return CV_8U;
    if (dt == "u16") return CV_16U;
    if (dt == "i32") return CV_32S;
    if (dt == "f32") return CV_32F;
    if (dt == "f64") return CV_64F;
    return -1;
}

// READ view over a consumer's INPUT `xi/image` blob: a non-owning cv::Mat over
// the blob payload, typed CV_MAKETYPE(depth(dt), channels), packed stride. Empty
// Mat (a loud cv:: error downstream, not silent corruption) when the dtype is
// unrepresentable or the view is malformed. The Mat must not outlive the pack the
// blob was read from (its bytes alias pool memory). Like as_cv_read(Image), the
// const& parameter is the "I am reading this" signal.
inline cv::Mat as_cv_read(const ImageBlobView& b) {
    const int depth = cv_depth_for_dt(b.dt);
    if (depth < 0 || !b.ok()) return {};
    const int type = CV_MAKETYPE(depth, b.channels);
    return cv::Mat(b.height, b.width, type,
                   const_cast<uint8_t*>(b.payload.data()),
                   static_cast<size_t>(b.width) * b.channels * b.elem_size());
}
inline cv::Mat as_cv(const ImageBlobView& b) { return as_cv_read(b); }

// WRITE view over a freshly MINTED `xi/image` blob payload — the producer's
// zero-copy mint window. `payload` is the pointer blob_mint / mint_image handed
// back; write the output pixels straight into the returned cv::Mat, then
// adopt_blob and drop the mint ref. Empty Mat on an unrepresentable dtype or a
// bad shape.
//
// CONTRACT (mirrors as_cv_write(Image), which returns empty on a non-writable
// input view): call this ONLY on a payload you just minted and have NOT yet
// adopted — a uniquely-owned (pool rc == 1) buffer. Once a blob is adopted/sealed
// its buffer may be shared with other consumers; writing it then corrupts their
// view. A raw payload pointer carries no writability flag, so this gate is the
// caller's to keep — the mint→fill→adopt ordering enforces it structurally.
inline cv::Mat as_cv_write_blob(void* payload, int32_t w, int32_t h, int32_t c,
                                std::string_view dt) {
    const int depth = cv_depth_for_dt(dt);
    if (depth < 0 || !payload || w <= 0 || h <= 0 || c <= 0) return {};
    const int type = CV_MAKETYPE(depth, c);
    return cv::Mat(h, w, type, payload,
                   static_cast<size_t>(w) * c * image_blob_dt_elem_size(dt));
}

// Encode an Image to a compressed buffer (".jpg"/".jpeg"/".png"). Bakes in the
// RGB->BGR flip OpenCV's encoders expect, so an image built from an RGB overlay
// comes out with the right colours (DM-2 / DM-12). Generic — no "preview" notion.
inline std::vector<unsigned char> encode_image(const Image& img,
                                               const std::string& ext = ".jpg",
                                               int quality = 85) {
    cv::Mat enc;
    cv::Mat rgb = as_cv_mat(img);
    if (rgb.channels() == 3) cv::cvtColor(rgb, enc, cv::COLOR_RGB2BGR);
    else                     enc = rgb;
    std::vector<unsigned char> buf;
    std::vector<int> params;
    if (ext == ".jpg" || ext == ".jpeg") params = { cv::IMWRITE_JPEG_QUALITY, quality };
    cv::imencode(ext, enc, buf, params);
    return buf;
}

} // namespace xi
