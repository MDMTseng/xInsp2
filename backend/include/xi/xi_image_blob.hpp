#pragma once
//
// xi_image_blob.hpp — the CONSUMER-side reader for the `xi/image` convention
// type carried in a self-describing Pack blob's descriptor (spec 30,
// docs/new_gen/30-self-describing-blob-plane.md).
//
// A producer mints an `xi/image` blob with the descriptor
// {"t":"xi/image","w","h","c","dt"} (xi::make_image_desc / ScriptPackBuilder::
// add_image_blob / a hand-rolled xi::mp::Writer). This header is the honest
// read side: parse that descriptor + its payload into an ImageBlobView — logical
// shape {w,h,c} + the "dt" element type + the payload span — so a consumer gets
// one fail-loud call instead of hand-decoding the descriptor or bouncing through
// the @1 xi_pack_image adapter (which assumes u8).
//
// It is OpenCV-FREE and Pack-FREE — it reads a descriptor span + a payload span
// (from PackIn::blob / ScriptPack::get_blob / the @4 get_blob C slot), so it is
// usable on the plugin side, the script side, and host-side. The cv::Mat sugar
// over an ImageBlobView lives in the opt-in <xi/xi_cv.hpp>; the SDK accessors
// PackIn::image_blob / ScriptPack::image_blob wrap this over their get_blob.
//
#include "xi_image_dt.hpp"  // xi::image_blob_dt_elem_size (the ONE dtype table)
#include "xi_mp.hpp"        // xi::mp::Reader / Element / Kind (canonical decode)
#include "xi_mp_build.hpp"  // xi::mp::skip_value / as_i64 (read helpers)

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace xi {

// A parsed `xi/image` self-describing blob: logical shape {w,h,c}, the element
// dtype string, and the payload span (borrowed — valid as long as the caller's
// desc/payload spans are). Zero-copy: `payload` aliases the pool buffer's bytes.
struct ImageBlobView {
    int32_t width = 0, height = 0, channels = 0;
    std::string_view dt;                 // "u8"|"u16"|"i32"|"f32"|"f64"
    std::span<const uint8_t> payload;    // w*h*c*elem_size(dt) element bytes
    int64_t  elem_size() const { return image_blob_dt_elem_size(dt); }
    bool     ok()        const {
        return width > 0 && height > 0 && channels > 0 &&
               !dt.empty() && payload.data() != nullptr;
    }
};

// Parse an `xi/image` descriptor map (canonical msgpack, string keys) + its
// payload into an ImageBlobView. FAIL-LOUD (nullopt), never a silent reinterpret,
// when: `desc` is not a map; "t" is absent or != "xi/image"; a required key
// (w/h/c/dt) is missing or mistyped; the dtype is unsupported; a dim is
// non-positive; or the payload is shorter than w*h*c*elem_size(dt). Unknown
// descriptor keys (a toolbox's own) are skipped, not rejected — the core reads
// only this convention's keys. ONE exception: "loc" (a RESERVED key,
// docs/roadmap/memory-domains.md) — a non-"host" loc means the payload is a
// DEVICE-MEMORY PROXY, not pixels; a host-only reader must refuse it here,
// loudly, instead of wrapping an empty/garbage span.
inline std::optional<ImageBlobView> read_image_blob(std::span<const uint8_t> desc,
                                                    std::span<const uint8_t> payload) {
    mp::Reader r(desc.data(), desc.size());
    mp::Element m;
    if (r.next(m) != mp::Status::Ok || m.kind != mp::Kind::Map) return std::nullopt;

    std::string_view t, dt;
    int64_t w = 0, h = 0, c = 0;
    bool have_w = false, have_h = false, have_c = false;
    for (uint32_t i = 0; i < m.len; ++i) {
        mp::Element k;
        if (r.next(k) != mp::Status::Ok || k.kind != mp::Kind::Str) return std::nullopt;
        std::string_view key(reinterpret_cast<const char*>(k.data), k.len);
        if (key == "loc") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok || v.kind != mp::Kind::Str) return std::nullopt;
            std::string_view s(reinterpret_cast<const char*>(v.data), v.len);
            if (s != "host") return std::nullopt;   // device proxy: not readable here
        } else if (key == "t" || key == "dt") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok || v.kind != mp::Kind::Str) return std::nullopt;
            std::string_view s(reinterpret_cast<const char*>(v.data), v.len);
            if (key == "t") t = s; else dt = s;
        } else if (key == "w" || key == "h" || key == "c") {
            mp::Element v;
            if (r.next(v) != mp::Status::Ok) return std::nullopt;
            auto iv = mp::as_i64(v);
            if (!iv) return std::nullopt;
            if (key == "w") { w = *iv; have_w = true; }
            else if (key == "h") { h = *iv; have_h = true; }
            else { c = *iv; have_c = true; }
        } else {
            if (!mp::skip_value(r)) return std::nullopt;   // unknown key: skip its value
        }
    }

    if (t != "xi/image" || dt.empty() || !have_w || !have_h || !have_c) return std::nullopt;
    if (w <= 0 || h <= 0 || c <= 0) return std::nullopt;
    const int64_t elem = image_blob_dt_elem_size(dt);
    if (elem == 0) return std::nullopt;                    // dtype the caller can't represent
    const int64_t need = w * h * c * elem;
    if (need <= 0 || static_cast<int64_t>(payload.size()) < need) return std::nullopt;

    ImageBlobView v;
    v.width = static_cast<int32_t>(w);
    v.height = static_cast<int32_t>(h);
    v.channels = static_cast<int32_t>(c);
    v.dt = dt;
    v.payload = payload.first(static_cast<size_t>(need));
    return v;
}

}  // namespace xi
