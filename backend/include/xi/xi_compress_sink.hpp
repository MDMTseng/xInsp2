#pragma once
//
// xi_compress_sink.hpp — indirection so a plugin's host_api->compress_image()
// can reach the backend's JPEG encoder + cache without xi_core depending on the
// backend's image-codec layer (opencv / turbojpeg / stb).
//
// Same pattern as xi_status_sink / xi_binary_sink: make_host_api() (in xi_core)
// wires api.compress_image to call through this sink; the backend (service_main)
// installs the real encoder, which keeps a process-global N-rotate cache keyed by
// a content hash of the pixels — so the SAME image compressed by several plugins
// (or repeatedly) is JPEG-encoded ONCE. No sink installed ⇒ compress_image
// returns 0 (the plugin then encodes itself, if it can).
//
#include <cstdint>

namespace xi {

// (pixels, w, h, channels, quality, out, out_cap) -> bytes written, or -needed
// if out_cap is too small, or 0 on error / no encoder installed.
using CompressSinkFn = int32_t (*)(const void* pixels, int32_t w, int32_t h,
                                   int32_t channels, int32_t quality,
                                   void* out, int32_t out_cap);

inline CompressSinkFn& compress_sink() {
    static CompressSinkFn fn = nullptr;
    return fn;
}

} // namespace xi
