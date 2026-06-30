#pragma once
//
// xi_cv.hpp — OpenCV convenience helpers with xInsp2's RGB convention baked in.
//
// xi::Image stores pixels RGB-ordered; OpenCV defaults to BGR. These wrap the
// conversions so each plugin doesn't re-derive them (was DM-12: every plugin
// hand-rolled image<->Mat copies and re-remembered the RGB->BGR-before-encode
// rule). For the zero-copy path use Image::as_cv_mat() (non-owning view) and
// Plugin::pool_image() directly — see docs/guides/write-a-plugin.md.
//
#include "xi_image.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

#include <string>
#include <vector>

namespace xi {

// Owning cv::Mat COPY of an Image (RGB order, like the bytes). Use this when you
// need a Mat that outlives the Image; for an in-place op prefer the zero-copy
// non-owning `Image::as_cv_mat()`.
inline cv::Mat to_cv(const Image& img) { return img.as_cv_mat().clone(); }

// cv::Mat -> owning xi::Image. The Mat must already be in xInsp2's RGB order
// (e.g. one produced by as_cv_mat / to_cv, NOT a raw cv::imread which is BGR).
// Naming-symmetric alias of from_cv_mat().
inline Image to_image(const cv::Mat& m) { return from_cv_mat(m); }

// Encode an Image to a compressed buffer (".jpg"/".jpeg"/".png"). Bakes in the
// RGB->BGR flip OpenCV's encoders expect, so an image built from an RGB overlay
// comes out with the right colours (DM-2 / DM-12). Generic — no "preview" notion.
inline std::vector<unsigned char> encode_image(const Image& img,
                                               const std::string& ext = ".jpg",
                                               int quality = 85) {
    cv::Mat enc;
    cv::Mat rgb = img.as_cv_mat();
    if (rgb.channels() == 3) cv::cvtColor(rgb, enc, cv::COLOR_RGB2BGR);
    else                     enc = rgb;
    std::vector<unsigned char> buf;
    std::vector<int> params;
    if (ext == ".jpg" || ext == ".jpeg") params = { cv::IMWRITE_JPEG_QUALITY, quality };
    cv::imencode(ext, enc, buf, params);
    return buf;
}

} // namespace xi
