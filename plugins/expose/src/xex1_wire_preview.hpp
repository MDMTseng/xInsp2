// xex1_wire_preview.hpp — the WS-wire PREVIEW dump walk (E2). expose's SEND
// path ONLY.
//
// THE SPLIT (see the E2 design + xex1_pack_dump.hpp banner):
//   * xex1_pack_dump.hpp::encode_pack_v3  — the ONE generic RAW pack->v3 dump,
//     SHARED by every sink (expose's pull/store + record_save's disk persist).
//     It inlines images as raw `bin` px. This header does NOT touch it, so the
//     memory ≈ disk identity (record->replay byte-green) is preserved.
//   * encode_pack_v3_wire (HERE)          — expose's live-wire variant. Identical
//     to encode_pack_v3 for every non-image entry, but for IMAGE entries it asks
//     an encoder for a full-resolution compressed preview and emits it via the
//     V3Entry.preview substitution (raw px leaves the wire; a nested `preview`
//     map rides beside it). Only expose.cpp includes this header; record_save
//     never sees it.
//
// FAIL-OPEN, PER IMAGE (never to nothing):
//   The encoder returns a PreviewOutcome per image:
//     * Compressed  — a full-res JPEG; the entry carries the `preview` map.
//     * RawFallback — this input could not be previewed (contract $fault / empty
//                     jpeg / null result); the entry falls back to RAW px. Other
//                     images in the same frame are unaffected.
//     * CodecDown   — the capability funnel signalled a codec-wide failure
//                     (negative rc: quarantined / unknown / crashed). This image
//                     falls to RAW px; the CALLER flips to persistent degraded
//                     mode so subsequent frames skip the cap entirely (no retry
//                     storm — contract fact 3).
#ifndef XI_XEX1_WIRE_PREVIEW_HPP
#define XI_XEX1_WIRE_PREVIEW_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <xi/xi_abi.hpp>   // xi::PackIn + xi::Record::kChannelKey/kSeqKey
#include <xi/xi_mp.hpp>    // xi::mp::Writer (non-image entry re-encode)

#include "xex1_encode.hpp"  // xi::xex1::V3Entry / encode_frame_v3 (shared encoder)

namespace xi {
namespace xex1 {

// The per-image encode verdict the wire walk acts on.
enum class PreviewOutcome { Compressed, RawFallback, CodecDown };

// Walk a sealed pack to the canonical XEX1-v3 frame, substituting a compressed
// preview for each IMAGE entry's raw pixels. `enc` is:
//   PreviewOutcome enc(const uint8_t* px, int w, int h, int c,
//                      int& q_out, std::vector<uint8_t>& jpeg_out);
// On Compressed it must fill jpeg_out (non-empty) and q_out. Any other outcome
// leaves the image RAW on the wire. `codec_down_out`/`raw_fallback_out` report
// whether any image hit CodecDown / RawFallback so the caller can update state.
template <class EncodeFn>
inline std::vector<uint8_t> encode_pack_v3_wire(const xi::PackIn& in,
                                                std::string_view channel,
                                                uint64_t seq,
                                                EncodeFn&& enc,
                                                bool* codec_down_out = nullptr,
                                                bool* raw_fallback_out = nullptr) {
    if (codec_down_out)   *codec_down_out = false;
    if (raw_fallback_out) *raw_fallback_out = false;

    std::vector<xi::xex1::V3Entry> entries;
    const int n = in.count();
    entries.reserve((size_t)(n > 0 ? n : 0));
    // Owns every compressed buffer for the lifetime of encode_frame_v3 below;
    // V3Entry.pv_jpeg points INTO it, so it must not reallocate — reserve up front.
    std::vector<std::vector<uint8_t>> jpeg_store;
    jpeg_store.reserve((size_t)(n > 0 ? n : 0));

    for (int i = 0; i < n; ++i) {
        auto keyv = in.key_at(i);
        if (!keyv) continue;
        std::string key(*keyv);
        if (key == xi::Record::kChannelKey || key == xi::Record::kSeqKey) continue;
        const int tag = in.tag_at(i);
        xi::xex1::V3Entry e;
        e.key = key;
        e.tag = (uint8_t)tag;
        switch (tag) {
            case XI_PACK_TAG_I64: {
                xi::mp::Writer w; w.int_(in.i64_or(key.c_str(), 0)); e.value = w.take();
                break;
            }
            case XI_PACK_TAG_F64: {
                xi::mp::Writer w; w.float_(in.f64(key.c_str()).value_or(0.0)); e.value = w.take();
                break;
            }
            case XI_PACK_TAG_BOOL: {
                xi::mp::Writer w; w.boolean(in.boolean(key.c_str()).value_or(false)); e.value = w.take();
                break;
            }
            case XI_PACK_TAG_STR: {
                xi::mp::Writer w; w.str(in.str(key.c_str()).value_or("")); e.value = w.take();
                break;
            }
            case XI_PACK_TAG_BIN: {
                auto b = in.bin(key.c_str());
                xi::mp::Writer w;
                if (b) w.bin(b->first, (size_t)b->second); else w.bin(nullptr, 0);
                e.value = w.take();
                break;
            }
            case XI_PACK_TAG_MP: {
                auto m = in.mp(key.c_str());
                if (m) e.value.assign(m->first, m->first + m->second);
                break;
            }
            case XI_PACK_TAG_IMAGE: {
                auto im = in.image(key.c_str());
                if (!im || !im->pixels) continue;
                e.w = im->width; e.h = im->height; e.c = im->channels;
                e.px = static_cast<const uint8_t*>(im->pixels);
                e.px_len = im->length > 0 ? (size_t)im->length : 0;

                // The preview substitution — the ONE line that differs from the
                // shared raw dump. Fail-open, per image.
                int q = 0;
                std::vector<uint8_t> jpeg;
                const PreviewOutcome oc = enc(e.px, e.w, e.h, e.c, q, jpeg);
                if (oc == PreviewOutcome::Compressed && !jpeg.empty()) {
                    jpeg_store.push_back(std::move(jpeg));
                    const std::vector<uint8_t>& owned = jpeg_store.back();
                    e.preview = true;
                    e.pv_q    = q;
                    e.pv_jpeg = owned.data();
                    e.pv_len  = owned.size();
                } else {
                    // RawFallback / CodecDown / empty jpeg -> emit RAW px (e.px
                    // already set). Never drop the image.
                    if (oc == PreviewOutcome::CodecDown && codec_down_out) *codec_down_out = true;
                    if (raw_fallback_out) *raw_fallback_out = true;
                }
                break;
            }
            default: continue;   // unknown tag: skip (opaque forward-compat)
        }
        entries.push_back(std::move(e));
    }
    return xi::xex1::encode_frame_v3(channel, seq, entries);
}

}  // namespace xex1
}  // namespace xi

#endif  // XI_XEX1_WIRE_PREVIEW_HPP
