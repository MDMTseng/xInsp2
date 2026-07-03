#pragma once
//
// xi_jpeg_cap.hpp — delegate a preview JPEG encode to the "xi.jpeg.encode"
// capability (the xi.imgcodec lib plugin), the ENCODE half of the polaris2
// CORE-CODEC EVICTION. At v12 (THE CUT) this is the ONLY encode engine the
// backend has: the in-core encode fallback in the host's compress_sink is
// deleted, so a miss here is a hard fail (the sink returns 0), not a fall-back
// to xi::encode_jpeg. The SIBLING of xi_image_io.cpp's read_via_capability
// (the decode half) — same forwarding funnel, same discipline:
//
//   * per-call availability re-check (the encoder can register AFTER a caller
//     starts — absent-then-present — and vanish again on unload; never cache
//     absence),
//   * self-serve reentrancy refusal handled by the funnel (-5): the encoder is
//     never served by itself,
//   * SEH fault attribution to the lib instance inside f_cap_call (the funnel),
//   * quarantine fail-fast (-3) and contract $fault → returns false; with the
//     in-core fallback gone the compress_sink then returns 0 (no bytes).
//
// CACHE: this funnel does NOT cache. When the capability serves, the dedup is
// imgcodec's FNV-1a content cache. One cache per served path — no double-
// caching (see service_main compress_sink).
//
// NOTE: xi_jpeg.hpp (xi::encode_jpeg) is NOT deleted at v12 — the imgcodec lib
// plugin still consumes it (with turbojpeg) as its own encoder. It is simply no
// longer reached from the backend's compress path (docs/new_gen/14 / doc 10).
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
// $fault, or a malformed reply. At v12 there is no in-core fallback: the caller
// (compress_sink) treats false as a hard failure and returns 0.
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
