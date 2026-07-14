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

// THE ONE blob validation seam (spec 30). Fail-loud: magic, desc_len in bounds,
// canonical-map descriptor, payload_off ≤ len. Used by adopt_blob/get_blob (host
// side) AND the wire parser (ingress side). Returns true iff `base`/`len` is a
// well-formed self-describing blob buffer.
inline bool blob_head_validate(const uint8_t* base, size_t len) {
    if (!base || len < 8) return false;
    if (pack_mp_detail::get_u32_le(base) != kBlobMagic) return false;
    uint32_t desc_len = pack_mp_detail::get_u32_le(base + 4);
    if (uint64_t(8) + desc_len > len) return false;                 // desc overrun
    if (!blob_desc_is_canonical_map(base + 8, desc_len)) return false;
    return blob_payload_off(desc_len) <= len;                       // payload_off in bounds
}

}  // namespace xi
