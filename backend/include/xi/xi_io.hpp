#pragma once
//
// xi_io.hpp — host-mediated file I/O + per-run context for inspection
// scripts.
//
// Scripts that need to read a frame from disk (typical AI-driven
// workflow: agent points the backend at a PNG and asks for a result)
// use:
//
//   #include <xi/xi.hpp>      // pulls in xi_io.hpp
//
//   void xi_inspect_entry(int) {
//       auto frame = xi::imread(xi::current_frame_path());
//       VAR(input, frame);
//       ...
//   }
//
// Both pieces are necessary:
//   - `current_frame_path()` reads the value the host sets per
//     `cmd:run` via the `frame_path` arg.
//   - `imread()` decodes PNG / JPEG / BMP / TGA / GIF / PSD / HDR / PIC
//     into an xi::Image. Goes through host_api so the host's
//     stb_image is the single decoder; the script DLL doesn't have to
//     vendor / link it.
//
// These accessors are script-only — they reach into globals defined
// by xi_script_support.hpp (force-included into every script DLL).
// Plugins never include xi.hpp / xi_io.hpp, so the externs below
// don't bite plugin / backend translation units.
//

#include "xi_abi.h"
#include "xi_image.hpp"

#include <cstring>
#include <string>

extern char  g_run_frame_path_[];   // xi_script_support.hpp
extern void* g_use_host_api_;       // xi_script_support.hpp (xi_host_api*)

namespace xi {

inline std::string current_frame_path() {
    return std::string(g_run_frame_path_);
}

// Read a file into an xi::Image. Empty Image on any failure (file
// missing, unsupported format, decode error). Decoder runs on the
// host side (via host_api->read_image_file, which is stb_image).
// **Pixel order is RGB**, NOT OpenCV's default BGR — when handing
// the resulting `as_cv_mat()` to cv::cvtColor, use `cv::COLOR_RGB2*`,
// not `BGR2*`. (Getting this wrong is a silent failure: red discs
// would map to where blue ones should be in HSV space.)
// Pixels are copied into a fresh xi::Image so the script's image
// lifetime is independent
// of the host pool.
inline Image imread(const std::string& path) {
    auto* host = static_cast<const xi_host_api*>(g_use_host_api_);
    if (!host || !host->read_image_file) return {};
    xi_image_handle h = host->read_image_file(path.c_str());
    if (!h) return {};
    int w  = host->image_width(h);
    int ht = host->image_height(h);
    int c  = host->image_channels(h);
    const uint8_t* src = host->image_data(h);
    Image img;
    if (src && w > 0 && ht > 0 && c > 0) img = Image(w, ht, c, src);
    host->image_release(h);
    return img;
}

} // namespace xi
