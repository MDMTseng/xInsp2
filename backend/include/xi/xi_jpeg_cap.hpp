#pragma once
//
// xi_jpeg_cap.hpp — delegate a preview JPEG encode to the "xi.jpeg.encode"
// capability (the xi.imgcodec lib plugin), the ENCODE half of the polaris2
// CORE-CODEC EVICTION (stage 2 of 2, v11-compatible). The SIBLING of
// xi_image_io.cpp's read_via_capability (the decode half) — same forwarding
// funnel, same discipline:
//
//   * per-call availability re-check (the encoder can register AFTER a caller
//     starts — absent-then-present — and vanish again on unload; never cache
//     absence),
//   * self-serve reentrancy refusal handled by the funnel (-5): the encoder is
//     never served by itself,
//   * SEH fault attribution to the lib instance inside f_cap_call (the funnel),
//   * quarantine fail-fast (-3) and contract $fault → the caller falls back to
//     the built-in in-core encoder (xi_jpeg.hpp), byte-for-byte the pre-eviction
//     path.
//
// CACHE: this funnel does NOT cache. The host's compress_sink keeps its content-
// hash memo cache for the IN-CORE fallback ONLY; when the capability serves, the
// dedup is imgcodec's identical FNV-1a content cache (same key algorithm). One
// cache per served path — no double-caching (see service_main compress_sink).
//
// At v12 the in-core fallback (xi_jpeg.hpp) and BACKEND-target XINSP2_HAS_TURBOJPEG
// are deleted; the capability becomes the only encode engine and turbojpeg lives
// solely in the imgcodec lib plugin (docs/new_gen/14 roster / doc 10 cut).
//
#include <xi/xi_image.hpp>
#include <xi/xi_image_pool.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace xi {

// Try to encode `img` to JPEG `out` through the registered "xi.jpeg.encode"
// capability. Returns false when NOT served — capability plane not installed,
// capability unregistered, funnel refusal (quarantine -3, reentrancy -5),
// handler crash -2 (already charged to the LIB instance by the funnel), contract
// $fault, or a malformed reply — and the caller falls back to the in-core encoder.
inline bool encode_via_capability(const Image& img, int quality,
                                  std::vector<uint8_t>& out) {
    out.clear();
    if (img.empty() || !img.data()) return false;
    if (img.channels != 1 && img.channels != 3 && img.channels != 4) return false;

    // The plane rides the same published slots get_interface serves — null on a
    // host without the pack/cap planes (headless unit tests): fallback.
    const auto* cap = static_cast<const xi_cap_v1*>(
        ImagePool::cap_iface_slot().load(std::memory_order_acquire));
    const auto* pk = static_cast<const xi_pack_v1*>(
        ImagePool::pack_iface_slot().load(std::memory_order_acquire));
    if (!cap || !pk || !cap->available || !cap->call) return false;
    if (!cap->available("xi.jpeg.encode")) return false;   // re-probed per call

    // Image in → jpeg bin out. xi::Image is row-major contiguous interleaved
    // (w*h*c), the same layout the pack image type carries.
    xi_pack_builder b = pk->builder_new();
    pk->builder_add_image(b, "image", img.width, img.height, img.channels, img.data());
    pk->builder_add_i64(b, "quality", (int64_t)quality);
    xi_pack_handle in = pk->builder_seal(b);
    if (in == XI_PACK_NULL) return false;

    xi_pack_handle rep = XI_PACK_NULL;
    int32_t rc = cap->call("xi.jpeg.encode", in, &rep);
    pk->release(in);
    if (rc != XI_CAP_OK || rep == XI_PACK_NULL) return false;

    // A contract $fault (rejected request / encoder failure) → fallback; the
    // in-core encoder then produces the bytes exactly as pre-eviction.
    {
        const char* fp = nullptr; int32_t fl = 0;
        if (pk->get_str(rep, "$fault", &fp, &fl)) { pk->release(rep); return false; }
    }

    const void* jptr = nullptr; int32_t jlen = 0;
    if (!pk->get_bin(rep, "jpeg", &jptr, &jlen) || !jptr || jlen <= 0) {
        pk->release(rep);
        return false;
    }
    const auto* jb = static_cast<const uint8_t*>(jptr);
    out.assign(jb, jb + jlen);
    pk->release(rep);
    return true;
}

} // namespace xi
