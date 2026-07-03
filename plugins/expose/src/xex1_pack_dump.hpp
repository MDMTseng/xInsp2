// xex1_pack_dump.hpp — the ONE generic pack -> XEX1-v3 dump walk, shared by every
// pack SINK (expose's wire push, record_save's disk persist).
//
// xex1_encode.hpp owns the byte-level v3 encoder (encode_frame_v3, guarded by the
// protocol/fixtures goldens). This header sits one layer up: it walks a borrowed,
// sealed pack GENERICALLY (count()+key_at()+tag_at()+typed reads — zero producer
// knowledge, the doc 07 §2 self-description made real) and feeds the entries to
// that single encoder. Both expose.cpp and record_save.cpp call encode_pack_v3 so
// there is exactly ONE dump implementation: the wire frame expose pushes and the
// disk file record_save writes are the SAME bytes for the same pack (doc 07
// memory ≈ wire ≈ disk; doc 10 gate P3).
//
// Why here and not in xex1_encode.hpp: this walk needs the plugin-side SDK
// (xi::PackIn, xi::Record's reserved-key vocabulary), whereas xex1_encode.hpp is
// deliberately kept to xi_abi.h + xi_mp.hpp so the golden-fixture generator and
// the host-side identity test can include it without the plugin SDK. Keeping the
// PackIn walk in its own header preserves that separation.
#ifndef XI_XEX1_PACK_DUMP_HPP
#define XI_XEX1_PACK_DUMP_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <xi/xi_abi.hpp>   // xi::PackIn + xi::Record::kChannelKey/kSeqKey
#include <xi/xi_mp.hpp>    // the canonical max-width msgpack Writer (re-encode path)

#include "xex1_encode.hpp"  // xi::xex1::V3Entry / encode_frame_v3 (the shared encoder)

namespace xi {
namespace xex1 {

// Walk a sealed input pack and dump it to the canonical XEX1-v3 frame bytes.
// Scalars/str/bin are re-encoded through xi::mp::Writer (byte-identical to the
// pack arena — same canonical profile), nested msgpack rides verbatim, images
// inline their raw pool pixels as `bin` (doc 07 D2 export rule). The reserved
// $channel/$seq keys are LIFTED to the frame's top-level fields, not dumped as
// entries — the caller passes them in (already read from the pack).
inline std::vector<uint8_t> encode_pack_v3(const xi::PackIn& in,
                                           std::string_view channel,
                                           uint64_t seq) {
    std::vector<xi::xex1::V3Entry> entries;
    const int n = in.count();
    entries.reserve((size_t)(n > 0 ? n : 0));
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

#endif  // XI_XEX1_PACK_DUMP_HPP
