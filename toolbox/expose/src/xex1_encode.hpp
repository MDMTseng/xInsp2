// xex1_encode.hpp — the XEX1 frame's msgpack encoder, as a standalone header.
//
// This is the SINGLE SOURCE OF TRUTH for how the `expose` plugin serializes a
// record to the wire. It was lifted out of expose.cpp so the same encoder can be
// driven by the golden-fixture generator/round-trip test
// (toolbox/expose/tests/test_xex1_fixtures.cpp) — the frame bytes the plugin
// emits and the bytes the cross-implementation decoders (the plugin webUI's JS
// reader in ui/index.html, and qa/lib/xex1.py) are tested against are
// produced here and nowhere else.
//
// Only the msgpack subset the fixed XEX1 frame shape needs is implemented. Every
// width the encoder can emit has a boundary golden and a decoder cross-test:
//   uint : fixint / uint8 (0xCC) / uint16 (0xCD) / uint32 (0xCE) / uint64 (0xCF)
//   str  : fixstr / str8 (0xD9) / str16 (0xDA) / str32 (0xDB)
//   bin  : bin8 (0xC4) / bin16 (0xC5) / bin32 (0xC6)
//   arr  : fixarray / array16 (0xDC) / array32 (0xDD)
//   map  : fixmap / map16 (0xDE) / map32 (0xDF)
// Multi-byte integers are big-endian, per msgpack.
//
// XEX1-v3 (polaris2, docs/new_gen/07 + 10 gate P3) — the CANONICAL FRAME DUMP,
// FINALIZED. Where v1 is a Record-shaped display frame (json string + JPEG
// images), v3 is the frame's own bytes: a canonical max-width msgpack map built
// by xi::mp::Writer, carrying every entry's stored value VERBATIM (the small
// plane == the in-memory plane, byte-for-byte) plus images inlined as raw `bin`
// per doc 07 D1/D2. This is the memory ≈ wire ≈ disk surface. It is OPT-IN; v1
// stays the default wire and its bytes are untouched (its goldens still pin it).
// The v3 encoder is xi::mp's Writer — the hand-rolled v1 mp_* helpers above are
// used ONLY on the v1 path.
//
// v3 vs the v2 draft (the P1 bonus-finding fix): every frame entry now carries
// its XI_PACK_TAG_* as [tag, value] (a 2-element array). The v2 draft dumped the
// bare value, so on readback an IMAGE descriptor map {w,h,c,px} was
// indistinguishable from a genuine nested-msgpack map of the same shape — the
// loader had to GUESS by shape. v3 puts the tag on the wire (+14 bytes/entry in
// the max-width canonical profile: array32 header + int64 tag), so a loader
// recovers every entry's type exactly, never by shape. The tag rides max-width
// like every other value — a compact fixint would not survive the ingress
// canonicalizer byte-identically. The version field bumped 2 -> 3 so a v2-draft reader
// fails closed ("unsupported version") instead of silently misreading tagged
// entries; the tagless v2 draft never shipped beyond the polaris2 line.
#ifndef XI_XEX1_ENCODE_HPP
#define XI_XEX1_ENCODE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <xi/xi_abi.h>   // XI_PACK_TAG_* (the entry tag vocabulary v2 dumps by)
#include <xi/xi_mp.hpp>  // the canonical max-width msgpack Writer (v2 body codec)

namespace xi {
namespace xex1 {

using Buf = std::vector<uint8_t>;

inline void mp_u16(Buf& f, uint16_t v) { f.push_back(v >> 8); f.push_back(v & 0xFF); }
inline void mp_u32(Buf& f, uint32_t v) { f.push_back(v >> 24); f.push_back((v >> 16) & 0xFF);
                                         f.push_back((v >> 8) & 0xFF); f.push_back(v & 0xFF); }
inline void mp_u64(Buf& f, uint64_t v) { for (int s = 56; s >= 0; s -= 8) f.push_back((v >> s) & 0xFF); }

// fixmap / map16 / map32. Was fixmap-only (silently wrapped n>15 into the low
// nibble and corrupted the frame); now widens like arrays/strings do. A frame
// whose top-level map grows past 15 keys (forward-compatible extension fields)
// is now encodable and round-trips through the widened decoders.
inline void mp_map(Buf& f, uint32_t n) {
    if (n <= 15) f.push_back(0x80 | (uint8_t)n);
    else if (n <= 0xFFFF) { f.push_back(0xDE); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xDF); mp_u32(f, n); }
}
inline void mp_arr(Buf& f, uint32_t n) {
    if (n <= 15) f.push_back(0x90 | (uint8_t)n);
    else if (n <= 0xFFFF) { f.push_back(0xDC); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xDD); mp_u32(f, n); }
}
inline void mp_uint(Buf& f, uint64_t v) {
    if (v <= 0x7F) f.push_back((uint8_t)v);
    else if (v <= 0xFF) { f.push_back(0xCC); f.push_back((uint8_t)v); }
    else if (v <= 0xFFFF) { f.push_back(0xCD); mp_u16(f, (uint16_t)v); }
    else if (v <= 0xFFFFFFFFull) { f.push_back(0xCE); mp_u32(f, (uint32_t)v); }
    else { f.push_back(0xCF); mp_u64(f, v); }
}
inline void mp_str(Buf& f, const std::string& s) {
    size_t n = s.size();
    if (n <= 31) f.push_back(0xA0 | (uint8_t)n);
    else if (n <= 0xFF) { f.push_back(0xD9); f.push_back((uint8_t)n); }
    else if (n <= 0xFFFF) { f.push_back(0xDA); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xDB); mp_u32(f, (uint32_t)n); }
    f.insert(f.end(), s.begin(), s.end());
}
inline void mp_bin(Buf& f, const uint8_t* p, size_t n) {
    if (n <= 0xFF) { f.push_back(0xC4); f.push_back((uint8_t)n); }
    else if (n <= 0xFFFF) { f.push_back(0xC5); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xC6); mp_u32(f, (uint32_t)n); }
    f.insert(f.end(), p, p + n);
}

// One already-JPEG-encoded image entry (key + compressed bytes).
struct EncImage {
    std::string          key;
    std::vector<uint8_t> jpeg;
};

// Build one atomic XEX1 frame: magic 'XEX1' + a msgpack map
//   { v:1, channel, seq, json, images:[ {key, jpeg} ] , <extra...> }.
// `extra` appends forward-compatible top-level uint fields after `images`; it is
// what pushes a frame's top-level map past the 15-key fixmap boundary in the
// golden fixtures (real production frames pass none). Decoders ignore unknown
// keys, so an extended frame stays a valid XEX1 frame.
inline std::vector<uint8_t> encode_frame(
        const std::string& channel, uint64_t seq, const std::string& values_json,
        const std::vector<EncImage>& images,
        const std::vector<std::pair<std::string, uint64_t>>& extra = {}) {
    Buf f;
    f.push_back('X'); f.push_back('E'); f.push_back('X'); f.push_back('1');
    mp_map(f, 5 + (uint32_t)extra.size());
    mp_str(f, "v");       mp_uint(f, 1);
    mp_str(f, "channel"); mp_str(f, channel);
    mp_str(f, "seq");     mp_uint(f, seq);
    mp_str(f, "json");    mp_str(f, values_json);
    mp_str(f, "images");
    mp_arr(f, (uint32_t)images.size());
    for (const auto& im : images) {
        mp_map(f, 2);
        mp_str(f, "key");  mp_str(f, im.key);
        mp_str(f, "jpeg"); mp_bin(f, im.jpeg.data(), im.jpeg.size());
    }
    for (const auto& kv : extra) {
        mp_str(f, kv.first); mp_uint(f, kv.second);
    }
    return f;
}

// ---------------------------------------------------------------------------
// XEX1-v3 — the canonical frame dump (finalized: per-entry tags).
// ---------------------------------------------------------------------------

// One entry to dump. For a scalar/str/bin/mp entry, `value` is its ALREADY-
// CANONICAL msgpack bytes (one complete value) — the canonical walk's emit
// (host side: xi::Pack::canonical_value; since ④A the slab stores each inline
// payload AS its canonical value, so this is a verbatim splice of raw_at) OR
// re-encoded from the door's typed accessors (plugin side: a temp
// xi::mp::Writer). Either way they are byte-identical, because the walk and the
// Writer are one canonical-encode truth. For a BLOB entry (tag ==
// XI_PACK_TAG_BLOB, spec 30) `value` is empty and `blob`/`blob_len` point at the
// ENTIRE self-describing buffer (magic + desc_len + descriptor + pad + payload),
// emitted verbatim as one `bin` — memory == wire extends to blobs by
// construction (the wire arm IS the in-memory buffer).
struct V3Entry {
    std::string          key;
    uint8_t              tag = XI_PACK_TAG_MP;
    std::vector<uint8_t> value;              // canonical bytes (scalar/str/bin/mp)
    const uint8_t*       blob = nullptr;     // whole self-describing buffer (BLOB only)
    size_t               blob_len = 0;

    // --- WS-wire preview substitution (E2) — expose's SEND path ONLY ----------
    // When `preview` is set on an xi/image BLOB entry, the verbatim buffer leaves
    // the wire and the entry becomes a preview map instead:
    //   { "preview":{ "w","h","c", "enc":"jpeg", "q":<int>, "data":<jpeg bin> } }
    // — the SAME nested preview map the old IMAGE-tag path emitted (behavior-
    // equivalent), just under the BLOB tag. The FE reads dims + renders the JPEG
    // from it. record_save NEVER sets this (its walk is
    // xex1_pack_dump.hpp::encode_pack_v3, which leaves `preview` default-false),
    // so its disk bytes stay the verbatim buffer and memory ≈ disk is preserved.
    // TODO(preview-egress): the jpeg-compress/dedup/renderer-registry egress
    // design (CT) is not yet spec'd — this is the behavior-equivalent minimal
    // migration of the old IMAGE-tag preview.
    bool                 preview = false;
    int32_t              pv_w = 0, pv_h = 0, pv_c = 0;   // source dims (from the descriptor)
    int32_t              pv_q = 0;              // JPEG quality used
    const uint8_t*       pv_jpeg = nullptr;     // compressed bytes (must outlive encode)
    size_t               pv_len = 0;
};

// Build one XEX1-v3 frame: magic 'XEX1' + a canonical msgpack map
//   { "v":3, "channel":<str>, "seq":<int>,
//     "frame":{ <key>:[<tag>, <value>], ... } }.
// The "frame" sub-map is the GENERIC entry dump. Each entry is a 2-element
// array [tag, value]: `tag` is the entry's XI_PACK_TAG_* (fixint), `value` is
// the canonical bytes spliced VERBATIM (scalars/str/bin/mp) or an image
// descriptor map { "w":.., "h":.., "c":.., "px":<bin pixels> }. The tag is what
// lets a loader rebuild every entry's type EXACTLY (no shape-guessing — the v2
// draft's image-vs-map ambiguity is unrepresentable here).
// The whole body is 100% standard msgpack; any stock decoder reads it.
// `has_channel`/`has_seq` (default true) control whether the reserved
// "channel"/"seq" top-level fields are emitted. They default true so every
// existing caller (expose's wire, the fixtures) is byte-identical. record_save
// passes the pack's ACTUAL presence so a pack that never carried $channel/$seq
// round-trips WITHOUT them (F5) instead of gaining a spurious ""/0 on replay.
// A frame with both present is still the map(4) {v,channel,seq,frame} shape —
// unchanged bytes; only the never-present case shrinks the map.
inline std::vector<uint8_t> encode_frame_v3(
        std::string_view channel, uint64_t seq,
        const std::vector<V3Entry>& entries,
        bool has_channel = true, bool has_seq = true) {
    xi::mp::Writer w;
    // Seed the 'XEX1' magic INTO the Writer's buffer so the whole frame is built
    // in ONE buffer and returned by move — instead of building the msgpack body
    // then copying it into a fresh {'X','E','X','1'} + body vector (perf/ws-lean:
    // for a blob-dominant frame that prepend was a full 15–60 MB copy, HALF the
    // encode cost). The bytes are identical: magic followed by the same msgpack.
    w.raw_canonical(reinterpret_cast<const uint8_t*>("XEX1"), 4);
    w.map(2 + (has_channel ? 1u : 0u) + (has_seq ? 1u : 0u));
    w.key("v");       w.int_(3);
    if (has_channel) { w.key("channel"); w.str(channel); }
    if (has_seq)     { w.key("seq");     w.uint_(seq); }
    w.key("frame");   w.map((uint32_t)entries.size());
    for (const auto& e : entries) {
        w.key(e.key);
        w.array(2);
        w.uint_(e.tag);   // the entry's XI_PACK_TAG_* (canonical int64, like every scalar)
        if (e.tag == XI_PACK_TAG_BLOB) {
            if (e.preview) {
                // E2 WS-wire preview (xi/image blobs): the verbatim buffer leaves
                // the wire; the entry becomes { preview{w,h,c,enc,q,data} } — the
                // SAME nested preview map the old IMAGE path emitted, so the FE
                // and consumers read identical output. record_save never reaches
                // this branch.
                // TODO(preview-egress): deferred egress design hooks in here.
                w.map(1);
                w.key("preview");
                w.map(6);
                w.key("w");    w.int_(e.pv_w);
                w.key("h");    w.int_(e.pv_h);
                w.key("c");    w.int_(e.pv_c);
                w.key("enc");  w.str("jpeg");
                w.key("q");    w.int_(e.pv_q);
                w.key("data"); w.bin(e.pv_jpeg, e.pv_len);
            } else {
                // memory == wire: the ENTIRE self-describing buffer verbatim as
                // one canonical `bin` (magic + desc + pad + payload). A loader
                // validates the head (blob_head_validate) and rebuilds.
                w.bin(e.blob, e.blob_len);
            }
        } else {
            // Splice the entry's canonical value bytes onto the wire unchanged —
            // the memory ≈ wire identity (doc 07). One complete canonical value.
            w.raw_canonical(e.value.data(), e.value.size());
        }
    }
    return w.take();   // whole frame (magic + body) in one buffer — no prepend copy
}

}  // namespace xex1
}  // namespace xi

#endif  // XI_XEX1_ENCODE_HPP
