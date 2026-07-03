// xex1_pack_load.hpp — read a canonical XEX1-v3 dump back into a sealed xi::Pack.
//
// This is the HOST-side half of replay (doc 10 gate P3, persistence parity):
// xex1_pack_parse.hpp does the untrusted-disk parse (magic, ingress edge, tagged
// entry walk — plugin-safe); this header BUILDS the pack from the parse result
// via xi::PackBuilder (host-only: pool handles + PackRegistry are per-module
// singletons a plugin must not touch). The replay SOURCE plugin (record_replay)
// consumes the same parse and rebuilds through the host xi.pack@1 builder
// instead — ONE parse implementation, two build sites.
//
// FORMAT NOTE. XEX1-v3 carries every entry's XI_PACK_TAG_* on the wire as
// [tag, value], so the entry type is recovered EXACTLY — the v2 draft's
// image-descriptor shape guess is gone. Tag/value agreement (and image dim/px
// consistency) is enforced by the parser; by the time entries reach the builder
// they are trusted canonical values.
#ifndef XI_XEX1_PACK_LOAD_HPP
#define XI_XEX1_PACK_LOAD_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include <xi/xi_mp.hpp>    // xi::mp::Reader (decode the trusted canonical values)
#include <xi/xi_pack.hpp>  // xi::Pack / xi::PackBuilder (rebuild the sealed pack)

#include "xex1_pack_parse.hpp"  // parse_frame_v3 — the ONE untrusted-disk parse

namespace xi {
namespace xex1 {

// The outcome of a load. `pack` is meaningful only when ok(); channel/seq carry
// the reserved fields lifted back out of the frame header.
struct LoadResult {
    bool        ok = false;
    std::string error;      // human reason when !ok
    std::string channel;
    uint64_t    seq = 0;
    xi::Pack    pack;

    explicit operator bool() const { return ok; }
};

// Build a sealed host pack from a parsed v3 frame. The parse already proved
// tag/value agreement, so this is a straight typed rebuild.
inline LoadResult build_pack(ParsedFrame&& pf) {
    LoadResult out;
    if (!pf.ok) { out.error = std::move(pf.error); return out; }
    out.channel = std::move(pf.channel);
    out.seq     = pf.seq;

    xi::PackBuilder b;
    for (const ParsedEntry& e : pf.entries) {
        const uint8_t* vp = pf.at(e.off);
        const size_t   vn = e.len;
        switch (e.tag) {
            case XI_PACK_TAG_I64: {
                mp::Reader r(vp, vn); mp::Element v; r.next(v);
                b.add_i64(e.key, v.kind == mp::Kind::UInt ? (int64_t)v.u : v.i);
                break;
            }
            case XI_PACK_TAG_F64: {
                mp::Reader r(vp, vn); mp::Element v; r.next(v);
                b.add_f64(e.key, v.d);
                break;
            }
            case XI_PACK_TAG_STR: {
                mp::Reader r(vp, vn); mp::Element v; r.next(v);
                b.add_str(e.key, std::string_view((const char*)v.data, v.len));
                break;
            }
            case XI_PACK_TAG_BIN: {
                mp::Reader r(vp, vn); mp::Element v; r.next(v);
                b.add_bin(e.key, v.data, v.len);
                break;
            }
            case XI_PACK_TAG_MP:
                b.add_mp(e.key, vp, vn);   // nested msgpack (already through ingress)
                break;
            case XI_PACK_TAG_IMAGE:
                b.add_image(e.key, e.w, e.h, e.c, pf.at(e.px_off));
                break;
            default: break;   // unreachable: the parser refused unknown tags
        }
    }
    out.pack = b.seal();
    out.ok = true;
    return out;
}

// Read a canonical XEX1-v3 dump (magic 'XEX1' + msgpack body) back into a
// sealed pack. See xex1_pack_parse.hpp for the untrusted-disk discipline.
inline LoadResult load_pack_v3(const uint8_t* data, size_t size) {
    return build_pack(parse_frame_v3(data, size));
}

// File convenience: missing/unreadable file is a !ok() Result, not a throw.
inline LoadResult load_pack_v3_file(const std::string& path) {
    return build_pack(parse_frame_v3_file(path));
}

}  // namespace xex1
}  // namespace xi

#endif  // XI_XEX1_PACK_LOAD_HPP
