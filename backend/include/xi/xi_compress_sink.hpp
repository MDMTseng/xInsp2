#pragma once
//
// xi_compress_sink.hpp — indirection so a plugin's host_api->compress_image()
// can reach the backend's JPEG encoder without xi_core depending on the codec
// layer. This header only declares the sink TYPE + holder; the backend
// (service_main) installs the real function.
//
// Same pattern as xi_status_sink / xi_binary_sink: make_host_api() (in xi_core)
// wires api.compress_image to call through this sink; service_main installs the
// encoder. At v12 (THE CUT) that installed encoder is CAPABILITY-ONLY — it
// delegates to the "xi.jpeg.encode" capability (imgcodec) via
// encode_via_capability and, on a miss, returns 0. There is no in-core encode
// fallback (xi::encode_jpeg) and no host-side memo cache anymore; dedup is
// owned by imgcodec's content cache. No sink installed ⇒ compress_image
// returns 0 (the plugin then encodes itself, if it can).
//
// The in-core fallback BODY that this comment used to describe lives in
// service_main.cpp's compress_sink() lambda, NOT here — gutting it is a
// service_main edit, not a change to this header (the signature below is
// frozen; xi_image_pool.hpp's compress_image_impl calls through it verbatim).
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
