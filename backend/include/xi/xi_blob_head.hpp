#pragma once
//
// xi_blob_head.hpp — the self-describing blob head format + its ONE validation
// seam (spec 30, docs/new_gen/30-self-describing-blob-plane.md).
//
// A blob is a pool buffer whose head describes its own payload:
//
//     +0   u32  magic 'XBD1' (0x31444258 LE)   — fail-loud discriminator
//     +4   u32  desc_len                        — bytes of descriptor msgpack
//     +8   canonical msgpack map (the descriptor, string keys)
//     +8+desc_len … zero pad …
//     +payload_off = align_up(8 + desc_len, 64) — payload, 64B-aligned
//     +payload_off + payload_len = total buffer length
//
// This header carries ONLY the format constants + `blob_head_validate` and
// depends on nothing but the canonical msgpack codec (xi_mp.hpp). It is the
// seam "exported for the door and WIRE packages to reuse": xi_pack.hpp (the host
// container) includes it, AND the PLUGIN-SAFE wire parser (xex1_pack_parse.hpp,
// compiled into the record_replay source plugin) includes it — validating a blob
// on ingress WITHOUT pulling the host ImagePool/PackRegistry. One seam, one
// definition, both sides.
//
// The symbols live in their original namespaces (xi::/pack_mp_detail/pack_detail)
// so xi_pack.hpp's existing references are unchanged by the extraction.

#include "xi_mp.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace xi {

namespace pack_mp_detail {

// Little-endian u32 — the blob head's magic + desc_len are raw LE words (spec
// 30), distinct from the big-endian msgpack length fields (in xi_pack.hpp).
inline void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);       p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline uint32_t get_u32_le(const uint8_t* p) {
    return  uint32_t(p[0])        | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace pack_mp_detail

namespace pack_detail {

// align_up to a power-of-two boundary (blob payload offset / slab alignment).
inline constexpr uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

}  // namespace pack_detail

// 'XBD1' little-endian — the fail-loud blob discriminator.
inline constexpr uint32_t kBlobMagic = 0x31444258u;   // 'X''B''D''1' LE
inline constexpr uint64_t kBlobPayloadAlign = 64;     // payload 64B-aligned

// The offset of a blob's payload for a given descriptor length: the head
// (8 bytes) + descriptor, rounded up to the 64B payload alignment.
inline constexpr uint64_t blob_payload_off(uint32_t desc_len) {
    return pack_detail::align_up(uint64_t(8) + desc_len, kBlobPayloadAlign);
}

// Validate that `desc`/`desc_len` is a well-formed CANONICAL msgpack MAP with
// string keys (the descriptor contract). Reuses the ingress/mp machinery: the
// top element must be a Map, and canonicalize(desc) must reproduce the bytes
// byte-identically (which enforces canonical widths, string keys, no duplicate
// keys, no foreign ext, no trailing bytes). The core validates FORM only — it
// interprets no key. Pool-handle ext is rejected (reject_all policy) so a
// forged handle can never ride a descriptor.
inline bool blob_desc_is_canonical_map(const uint8_t* desc, uint32_t desc_len) {
    if (!desc || desc_len == 0) return false;
    mp::Reader peek(desc, desc_len);
    mp::Element e;
    if (peek.next(e) != mp::Status::Ok || e.kind != mp::Kind::Map) return false;
    mp::Writer out;
    if (mp::canonicalize(desc, desc_len, out) != mp::Status::Ok) return false;
    return out.size() == desc_len &&
           std::memcmp(out.bytes().data(), desc, desc_len) == 0;
}

// Cheap OFFSET-safety guard: magic + the length fields are in bounds so a
// consumer's desc/payload subspans cannot escape `len`. Does NOT re-canonicalize
// the descriptor. This is the READ-path check for a Blob whose head was already
// FULLY validated (blob_head_validate, below) at adopt/ingress and is immutable
// thereafter — re-canonicalizing on every get_blob is a per-frame tax (doc 28
// finding A④), while the offsets must still be re-derived safely from the head.
inline bool blob_head_bounds_ok(const uint8_t* base, size_t len) {
    if (!base || len < 8) return false;
    if (pack_mp_detail::get_u32_le(base) != kBlobMagic) return false;
    uint32_t desc_len = pack_mp_detail::get_u32_le(base + 4);
    if (uint64_t(8) + desc_len > len) return false;                 // desc overrun
    return blob_payload_off(desc_len) <= len;                       // payload_off in bounds
}

// THE ONE blob validation seam (spec 30). Fail-loud: magic, desc_len in bounds,
// payload_off ≤ len (blob_head_bounds_ok), AND a canonical-map descriptor. Used
// by adopt_blob and the wire parser (ingress side) — every route that admits a
// blob buffer FROM ANOTHER SOURCE. Returns true iff `base`/`len` is a well-formed
// self-describing blob buffer.
inline bool blob_head_validate(const uint8_t* base, size_t len) {
    if (!blob_head_bounds_ok(base, len)) return false;
    uint32_t desc_len = pack_mp_detail::get_u32_le(base + 4);
    if (!blob_desc_is_canonical_map(base + 8, desc_len)) return false;
    // The pad [8+desc_len, payload_off) MUST be zero. A minted blob always zeroes
    // it (mint_blob_impl), and it rides the wire VERBATIM (memory==wire), so a
    // non-zero pad is a non-canonical / forged buffer. Reject it fail-loud here
    // rather than silently normalizing it away on rebuild — that is what made the
    // "byte-lossless blob round-trip" claim untrue for hostile input (doc 28 B②).
    // bounds_ok guarantees payload_off <= len, so the range is in-bounds.
    const uint64_t payload_off = blob_payload_off(desc_len);
    for (uint64_t i = uint64_t(8) + desc_len; i < payload_off; ++i)
        if (base[i] != 0) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Padded sub-layout INSIDE a blob payload (a toolbox convenience, spec 30).
//
// A custom type often lays its payload out as
//     [ own info (msgpack / header bytes) ] [ pad ] [ bulk data ]
// with the BULK region aligned (SIMD / cv::Mat wrap / GPU upload). Because a
// blob payload base is 64B-aligned (kBlobPayloadAlign), aligning the bulk offset
// WITHIN the payload gives it absolute alignment for free. This helper does the
// offset math + the deterministic fill; it is convention-NEUTRAL — it writes no
// key and names nothing. The caller records `data_off` in its OWN descriptor if
// a reader needs to find the bulk region, e.g.:
//
//     auto L = xi::padded_layout(head.size(), bulk_bytes);   // 64B bulk align
//     if (!L) { /* fail loud: head too large / total overflow */ }
//     // mint a blob whose payload_len == L->total, with data_off in the desc:
//     //   {"t":"acme/scan","data_off":L->data_off, ...}
//     uint8_t* bulk = xi::place_padded_head(payload, head, *L);
//     // ... DMA / write bulk_bytes into `bulk` (64B-aligned) ...
//
// A reader parses the descriptor (data_off) and resolves the bulk as
// payload.data() + data_off.
struct PaddedLayout {
    uint32_t data_off = 0;   // offset of the bulk region within the payload
    size_t   total    = 0;   // total payload length (data_off + data_len)
};

// Compute the padded sub-layout for a [head][pad][bulk] payload. `align` must be
// a power of two (default: the blob payload alignment, so the bulk lands
// 64B-aligned in absolute terms). Fail-loud (nullopt) on: a zero / non-power-of-
// two align; a head whose aligned offset overflows uint32_t (the data_off field);
// or a total that overflows size_t. Pure arithmetic, no allocation.
inline std::optional<PaddedLayout> padded_layout(size_t head_len, size_t data_len,
                                                 size_t align = kBlobPayloadAlign) {
    if (align == 0 || (align & (align - 1)) != 0) return std::nullopt;   // power of two
    if (head_len > SIZE_MAX - (align - 1)) return std::nullopt;          // align_up would wrap
    const uint64_t data_off = pack_detail::align_up(uint64_t(head_len), uint64_t(align));
    if (data_off > 0xFFFFFFFFull) return std::nullopt;                   // fits data_off (u32)
    if (uint64_t(data_len) > uint64_t(SIZE_MAX) - data_off) return std::nullopt;  // total wraps
    PaddedLayout L;
    L.data_off = uint32_t(data_off);
    L.total    = size_t(data_off) + data_len;
    return L;
}

// Place the head into a minted `payload` and return a pointer to the bulk region.
// memcpy the `head` bytes at offset 0, ZERO the pad gap [head.size(), data_off),
// and return payload + data_off (64B-aligned iff `payload` is, which a minted
// blob payload is). `payload` must hold >= layout.total bytes, and head.size()
// must be <= layout.data_off (true when `layout` came from padded_layout(head.
// size(), ...)).
//
// KEEP ZEROED (wire determinism, NOT security): the pad gap rides the wire
// verbatim (memory == wire), so it must be deterministic. This zero stays even
// though the pool no longer zero-fills a minted buffer (CT ruling 2026-07) — the
// bulk region a producer fills may be uninitialised, but the pad never is.
inline uint8_t* place_padded_head(uint8_t* payload, std::span<const uint8_t> head,
                                  const PaddedLayout& layout) {
    if (!head.empty()) std::memcpy(payload, head.data(), head.size());
    if (layout.data_off > head.size())
        std::memset(payload + head.size(), 0, layout.data_off - head.size());
    return payload + layout.data_off;
}

}  // namespace xi
