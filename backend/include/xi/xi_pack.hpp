#pragma once
//
// xi_pack.hpp — the uniform keyed-buffer pack container, SLAB representation,
// SELF-DESCRIBING BLOB plane (decision ⑤-final,
// docs/new_gen/30-self-describing-blob-plane.md; supersedes the @3 image/tensor/
// type_id surface of doc 28 finding ⑤).
//
// A pack is ONE thing:  key(string) -> entry, where an entry is
// (type tag, const-span bytes). There is no image/tensor special case in the
// core: every non-scalar payload is a SELF-DESCRIBING BLOB — a pool buffer whose
// head describes its own payload. `xi/image` is just a convention type carried
// in a blob's descriptor. The core owns buffers and dispatch, not images.
//
// BLOB HEAD FORMAT (spec 30). A blob is a pool buffer whose base is 64B-aligned
// (pool guarantee):
//
//     +0   u32  magic 'XBD1' (0x31444258 LE)   — fail-loud discriminator
//     +4   u32  desc_len                        — bytes of descriptor msgpack
//     +8   canonical msgpack map (the descriptor, string keys)
//     +8+desc_len … zero pad …
//     +payload_off = align_up(8 + desc_len, 64) — payload, 64B-aligned
//     +payload_off + payload_len = total buffer length
//
//   * The descriptor is a CANONICAL msgpack map (same canonical rules as pack
//     ingress; validated fail-loud wherever foreign bytes arrive). The core
//     validates canonical form ONLY — it interprets no key. `"t"` (a type string,
//     namespaced by convention, e.g. "xi/image") is required BY CONVENTION and
//     read only as sugar (type_of); the core owns no type space.
//   * Alignment: base is 64B-aligned AND payload_off is a multiple of 64, so the
//     payload is always 64B-aligned (SIMD / cv::Mat wrap / GPU upload). Cost:
//     ≤ ~64-128B per blob — noise at MB scale, accepted at KB scale.
//   * Zero-copy: producers mint a described buffer FIRST (mint_blob writes the
//     head and exposes the aligned payload region), fill the payload in place
//     (camera DMA lands at base+payload_off), then adopt into a pack — zero
//     copies. add_blob copies bytes into a freshly minted buffer.
//   * ONE validation seam: blob_head_validate(base,len) — magic, desc_len
//     bounds, canonical-msgpack well-formedness of the descriptor map, and
//     payload_off ≤ len. Used by adopt_blob and exported for the door/wire
//     packages to reuse (same fail-loud discipline as the add_mp canonicalize
//     seam).
//
// REPRESENTATION (unchanged in shape from the slab migration):
//
//   One sealed pack = ONE contiguous SLAB + N pool-backed EXTERN buffers.
//
//   Slab layout (all offsets slab-relative):
//     [ PackHeader 64B ]
//     [ DirEntry * n, 32B each, sorted by (key_hash, key bytes, ordinal) ]
//     [ order table: uint32_t * n — order[ordinal] = directory index ]
//     [ payload: keys + entry bytes, bump-packed, per-entry aligned ]
//
//   * Lookup is a binary search on the hash-sorted directory; an equal-hash
//     run is memcmp-verified against the actual key bytes (collisions are
//     handled, never assumed away). No side index, no hash map, no per-entry
//     heap nodes — small AND large packs share one O(log n) path.
//   * INSERTION ORDER IS FIRST-CLASS: every DirEntry carries its insertion
//     ordinal and the slab carries the ordinal->dir-index order table, so
//     key_at/tag_at/for_each present exactly the order entries were added —
//     the contract the expose/record walkers depend on.
//   * INLINE entries store the entry's CANONICAL MSGPACK VALUE verbatim
//     (④A memory==wire): i64=int64 0xd3+8, f64=float64 0xcb+8 (NaN flattened at
//     add), bool=0xc2/0xc3, str=str32, small bin=bin32, nested msgpack (Mp)=its
//     canonical bytes. raw_at(i) IS the wire bytes; a typed read skips the
//     fixed-width header (one branch, zero copy).
//   * EXTERN entries (Bin >= kPackLargeThreshold, and every Blob) live in the
//     production ImagePool — owner-neutral mint through pack_pool below, pack
//     co-owns via the pool refcount. The slab payload holds a 16-byte ExtRecord
//     {handle, total_len} per EXTERN entry; the DirEntry points at it. A Blob's
//     total_len is the WHOLE self-describing buffer (head + desc + pad +
//     payload); a Bin's is its byte length.
//   * The slab buffer RECYCLES through the per-thread SlabPool and the builder's
//     staging vectors through a per-thread scratch pool, so a steady stream of
//     packs on one lane's thread is heap-free after warmup — the ImagePool
//     discipline preserved.
//
// WIRE / AT-REST FORMAT: memory == wire is RESTORED for inline entries (④A):
// the inline payload IS the canonical value, so the walk API (for_each_entry /
// canonical_value) SPLICES each inline entry's stored bytes verbatim rather than
// re-encoding. A Blob has no single scalar canonical value (its wire arm — the
// self-describing buffer verbatim — is the wire package's contract, package C);
// canonical_value returns false for a Blob, exactly as it did for the retired
// Image/Tensor tags.
//
// SEMANTICS KEPT: Pack is move-only, sealed-immutable, single-owner; borrowed
// views (get_str/get_bin/get_blob spans, key_at string_views) are valid for the
// life of the owning Pack. Drop-on-crash is EXACTLY destruction: the directory
// walk releases each pool handle once and the slab returns to the recycle pool.
//

#include "xi_abi.h"
#include "xi_image_pool.hpp"
#include "xi_mp.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xi {

// ===================================================================
// pack_mp_detail — adapters over the canonical codec (xi_mp.hpp).
//
//   xi::mp::Writer is the ONE canonical-encode truth; these fixed-width forms
//   emit/decode exactly the bytes it produces. Since ④A restored memory==wire
//   they are AGAIN the pack's inline STORAGE encoding: add_* encode the
//   canonical value into the slab at add-time (write_*), and the typed getters
//   decode the fixed-width header at a known offset (read_*). canonical_value
//   then splices the stored bytes verbatim — no re-encode.
//
//   Fixed-width forms only (int64 0xd3, float64 0xcb, str32 0xdb, bin32 0xc6)
//   — the canonical max-width profile of doc 07. Everything is 100% standard
//   msgpack; a stock decoder reads it. The blob-head u32 helpers below are
//   LITTLE-endian (the blob magic/desc_len are raw LE u32, NOT msgpack).
// ===================================================================
namespace pack_mp_detail {

inline uint64_t get_u64_be(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
inline void put_u32_be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);  p[3] = uint8_t(v);
}
inline uint32_t get_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}
// Little-endian u32 — the blob head's magic + desc_len are raw LE words (spec
// 30), distinct from the big-endian msgpack length fields above.
inline void put_u32_le(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);       p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline uint32_t get_u32_le(const uint8_t* p) {
    return  uint32_t(p[0])        | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Encoded-size constants for the canonical forms.
inline constexpr size_t kI64Size  = 9;   // 0xd3 + 8
inline constexpr size_t kF64Size  = 9;   // 0xcb + 8
inline constexpr size_t kBoolSize = 1;   // 0xc2 / 0xc3 — the canonical bool IS its tag byte
inline size_t str_size(size_t n) { return 5 + n; }  // 0xdb + 4 len + bytes
inline size_t bin_size(size_t n) { return 5 + n; }  // 0xc6 + 4 len + bytes

// The scalar encode scratch: one Writer per thread, CLEARED (capacity kept)
// per call, so after the first scalar on a thread this allocates nothing.
inline xi::mp::Writer& scalar_writer() {
    thread_local xi::mp::Writer w;
    w.clear();
    return w;
}

inline void write_i64(uint8_t* out, int64_t v) {
    xi::mp::Writer& w = scalar_writer();
    w.int_(v);                              // canonical int64 (0xd3), xi_mp's emit
    assert(w.size() == kI64Size);
    std::memcpy(out, w.bytes().data(), kI64Size);
}
inline void write_f64(uint8_t* out, double v) {
    xi::mp::Writer& w = scalar_writer();
    w.float_(v);                            // canonical float64 (0xcb) — flattens
    assert(w.size() == kF64Size);           // every NaN per ruling 1
    std::memcpy(out, w.bytes().data(), kF64Size);
}
inline void write_bool(uint8_t* out, bool v) {
    out[0] = v ? xi::mp::tag::True : xi::mp::tag::False;  // == Writer::boolean
}
inline void write_str(uint8_t* out, std::string_view s) {
    out[0] = xi::mp::tag::Str32;                          // == Writer::str header
    put_u32_be(out + 1, uint32_t(s.size()));
    if (!s.empty()) std::memcpy(out + 5, s.data(), s.size());
}
inline void write_bin(uint8_t* out, const void* data, size_t n) {
    out[0] = xi::mp::tag::Bin32;                          // == Writer::bin header
    put_u32_be(out + 1, uint32_t(n));
    if (n) std::memcpy(out + 5, data, n);
}

// Fixed-offset readers over TRUSTED canonical bytes (assert-only tag checks,
// zero validation cost, zero-copy views).
inline int64_t read_i64(const uint8_t* p) {
    assert(p[0] == xi::mp::tag::Int64 && "bytes are not a canonical int64");
    return int64_t(get_u64_be(p + 1));
}
inline double read_f64(const uint8_t* p) {
    assert(p[0] == xi::mp::tag::Float64 && "bytes are not a canonical float64");
    uint64_t bits = get_u64_be(p + 1);
    double v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}
inline bool read_bool(const uint8_t* p) {
    assert((p[0] == xi::mp::tag::False || p[0] == xi::mp::tag::True) &&
           "bytes are not a canonical bool");
    return p[0] == xi::mp::tag::True;
}
inline std::string_view read_str(const uint8_t* p) {
    assert(p[0] == xi::mp::tag::Str32 && "bytes are not a canonical str32");
    uint32_t n = get_u32_be(p + 1);
    return std::string_view(reinterpret_cast<const char*>(p + 5), n);
}
inline std::span<const uint8_t> read_bin(const uint8_t* p) {
    assert(p[0] == xi::mp::tag::Bin32 && "bytes are not a canonical bin32");
    uint32_t n = get_u32_be(p + 1);
    return std::span<const uint8_t>(p + 5, n);
}

} // namespace pack_mp_detail

namespace pack_detail {

// ===================================================================
// SlabPool — a per-thread freelist of recyclable slab buffers.
//
// A sealed pack's slab is BORROWED here at seal() and RETURNED here when the
// pack is destroyed, so a steady stream of packs built and dropped on one thread
// reuses the same handful of buffers instead of hitting the heap allocator per
// pack. Scoped THREAD-LOCAL (per-lane cache), so the recycle needs no lock. A
// pack that legitimately outlives its producer thread still frees correctly.
// Bounded (kMaxFree) so an idle thread does not hoard memory.
// ===================================================================
struct SlabBuf {
    std::unique_ptr<uint8_t[]> data;
    size_t cap = 0;
};

class SlabPool {
public:
    // Hand out a buffer with capacity >= need. Prefer a pooled one (LIFO — the
    // hottest in cache); otherwise allocate one of at least the default chunk.
    SlabBuf acquire(size_t need, size_t default_cap) {
        for (size_t i = free_.size(); i-- > 0;) {
            if (free_[i].cap >= need) {
                SlabBuf b = std::move(free_[i]);
                free_[i] = std::move(free_.back());
                free_.pop_back();
                return b;
            }
        }
        size_t cap = need > default_cap ? need : default_cap;
        return SlabBuf{std::unique_ptr<uint8_t[]>(new uint8_t[cap]), cap};
    }
    // Take a buffer back for reuse; drop it (free) if the pool is already full.
    void release(SlabBuf&& b) {
        if (b.data && free_.size() < kMaxFree) free_.push_back(std::move(b));
    }

private:
    static constexpr size_t kMaxFree = 32;
    std::vector<SlabBuf> free_;
};

// The SlabPool hides behind a trivially-destructible thread_local POINTER (the
// pixpool-magazine doctrine, xi_image_pool.hpp): the pointer slot itself has no
// destructor, so it stays readable (as null) even after the owning Owner
// thread_local has been destroyed — a Pack destroyed inside a LATER
// thread_local destructor (or during static teardown) falls through to a plain
// heap free instead of touching a destroyed free_ vector.
inline SlabPool* tls_slab_pool() {
    thread_local SlabPool* slot = nullptr;
    struct Owner {
        SlabPool** s;
        explicit Owner(SlabPool** slot_) : s(slot_) { *s = new SlabPool(); }
        ~Owner() { delete *s; *s = nullptr; }
    };
    thread_local Owner owner{&slot};
    return slot;
}
inline SlabBuf slab_acquire(size_t need, size_t default_cap) {
    if (SlabPool* p = tls_slab_pool()) return p->acquire(need, default_cap);
    size_t cap = need > default_cap ? need : default_cap;
    return SlabBuf{std::unique_ptr<uint8_t[]>(new uint8_t[cap]), cap};
}
inline void slab_release(SlabBuf&& b) {
    if (SlabPool* p = tls_slab_pool()) p->release(std::move(b));
}

// FNV-1a 64 over the key bytes — the directory sort/search key. Collisions
// are handled (equal-hash runs are memcmp-verified), never assumed away.
inline uint64_t hash_key(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (char c : s) { h ^= uint8_t(c); h *= 1099511628211ull; }
    return h;
}

// align_up to a power-of-two boundary (blob payload offset / slab alignment).
inline constexpr uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + (a - 1)) & ~(a - 1);
}

} // namespace pack_detail

// ===================================================================
// pack_pool — thin TYPELESS facade over ImagePool (unchanged boundary).
//
// The pool is image-shaped (create(w,h,ch)); a typeless buffer of N bytes is a
// (N,1,1) entry whose pixels ARE the bytes. The handle is minted here, released
// here, and resolved to a raw span. This is the ONLY mint path in the pack
// layer (doc 07's "handles are mintable only by the domain's own allocator").
//
// OWNER-NEUTRAL MINT (cross-plane owner-sweep data-loss fix): a buffer minted
// INTO a pack is governed by the pack alone. Minting owner-0 makes the image
// sweep skip pack buffers so the pack solely governs them (its directory walk
// releases each exactly once on destruction; a leaked pack is reclaimed by the
// PACK registry's own owner sweep).
//
// ZERO-FILL DISCIPLINE (doc 28 zeroinit verdict):
//   * alloc_bytes(src,n) is a COPY path — it full-memcpy's a non-null src over
//     the whole buffer, so the pool's zero-fill is pure waste; it mints an
//     UNINITIALISED buffer (create_uninit) and copies. A NULL src is a HARD
//     REJECT (fail-loud, returns XI_IMAGE_NULL) — a copy path with nothing to
//     copy is a caller bug, never a silently-zeroed buffer.
//   * alloc_canvas(n) is the WRITABLE-CANVAS path (mint_blob's payload region,
//     a producer fills it in place) — it KEEPS zero-fill (external contract:
//     an unwritten canvas byte reads as zero, and the head's pad bytes must be
//     zero). Uses the default create().
// ===================================================================
namespace pack_pool {

// Mint an UNINITIALISED typeless buffer of n bytes and copy src into it. src
// MUST be non-null (a copy path with a null src is a caller bug — fail-loud).
// 0 on failure (n==0, null src, over the pool's per-buffer cap, or exhausted).
inline xi_image_handle alloc_bytes(const void* src, size_t n) {
    if (n == 0 || n > size_t(INT32_MAX) || !src) return XI_IMAGE_NULL;  // null src: HARD REJECT
    ImagePool::OwnerGuard neutral(0);   // owner-neutral: pack-governed lifetime
    xi_image_handle h = ImagePool::instance().create_uninit(int32_t(n), 1, 1);
    if (h) std::memcpy(ImagePool::instance().data(h), src, n);
    return h;
}
// Mint a ZERO-FILLED writable canvas of n bytes (the producer fills it in
// place). 0 on failure. Keeps the create() zero-fill: unwritten canvas bytes
// (and, for a blob head, the pad between descriptor and payload) read as zero.
inline xi_image_handle alloc_canvas(size_t n) {
    if (n == 0 || n > size_t(INT32_MAX)) return XI_IMAGE_NULL;
    ImagePool::OwnerGuard neutral(0);   // owner-neutral: pack-governed lifetime
    return ImagePool::instance().create(int32_t(n), 1, 1);
}
inline void addref(xi_image_handle h) {
    if (h) ImagePool::instance().addref(h);
}
inline void release(xi_image_handle h) {
    // Safe even for a pack destroyed during static teardown: the pool
    // singleton is intentionally leaked (ImagePool::instance), never destroyed.
    if (h) ImagePool::instance().release(h);
}
// Writable data pointer (mint_blob writes the head + hands out the payload
// region). Owner-neutral pack buffers only.
inline uint8_t* writable(xi_image_handle h) {
    return h ? ImagePool::instance().data(h) : nullptr;
}
inline std::span<const uint8_t> view(xi_image_handle h) {
    if (!h) return {};
    auto& pool = ImagePool::instance();
    const uint8_t* p = pool.read_data(h);
    if (!p) return {};
    size_t n = size_t(pool.width(h)) * size_t(pool.height(h)) * size_t(pool.channels(h));
    return std::span<const uint8_t>(p, n);
}

} // namespace pack_pool

// ===================================================================
// Entry type tags. The tag is the entry's stored type; the payload is raw
// in-memory bytes (scalars/str/bin) or canonical msgpack (Mp), or an EXTERN
// pool-buffer reference (Bin above threshold, Blob). Unknown/opaque nested
// msgpack rides as Mp — forward compatibility by construction.
//
// Values are FROZEN for the surviving tags (I64=0..Bool=6, matching the frozen
// @1 door's XI_PACK_TAG_* in xi_abi.h). The retired Image(4) and Tensor(7) tags
// leave permanent gaps; Blob is APPENDED (=8), append-only discipline preserved.
// A Blob is ALWAYS EXTERN. (Wire/door tag reconciliation — XI_PACK_TAG_BLOB — is
// packages B/C.)
// ===================================================================
enum class PackTag : uint8_t {
    I64  = 0,
    F64  = 1,
    Str  = 2,
    Bin  = 3,
    Mp   = 5,
    Bool = 6,
    Blob = 8,
};

// Bytes at/above this size go to a pool buffer instead of the slab (D1 storage
// duality). Small enough that scalars/short strings stay inline; large enough
// that kilobyte metadata does not churn the pool.
inline constexpr size_t kPackLargeThreshold = 4096;

// ===================================================================
// The self-describing blob head (spec 30).
// ===================================================================

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
// canonical-map descriptor, payload_off ≤ len. Used by adopt_blob and exported
// for the door/wire packages to reuse. Returns true iff `base`/`len` is a
// well-formed self-describing blob buffer.
inline bool blob_head_validate(const uint8_t* base, size_t len) {
    if (!base || len < 8) return false;
    if (pack_mp_detail::get_u32_le(base) != kBlobMagic) return false;
    uint32_t desc_len = pack_mp_detail::get_u32_le(base + 4);
    if (uint64_t(8) + desc_len > len) return false;                 // desc overrun
    if (!blob_desc_is_canonical_map(base + 8, desc_len)) return false;
    return blob_payload_off(desc_len) <= len;                       // payload_off in bounds
}

// A minted, described pool buffer whose payload region is exposed for in-place
// fill (RAII: releases its own mint ref on drop). A producer mints, fills the
// payload, adopts into a builder (which addrefs), and lets the BufRef drop — the
// pack then holds the buffer alone. Move-only.
class BufRef {
public:
    BufRef() = default;
    BufRef(xi_image_handle h, uint8_t* payload, int64_t payload_len)
        : handle_(h), payload_(payload), payload_len_(payload_len) {}
    BufRef(BufRef&& o) noexcept { move_(std::move(o)); }
    BufRef& operator=(BufRef&& o) noexcept {
        if (this != &o) { reset_(); move_(std::move(o)); }
        return *this;
    }
    BufRef(const BufRef&) = delete;
    BufRef& operator=(const BufRef&) = delete;
    ~BufRef() { reset_(); }

    explicit operator bool() const { return handle_ != XI_IMAGE_NULL; }
    xi_image_handle handle()      const { return handle_; }
    uint8_t*        payload()     const { return payload_; }      // 64B-aligned, writable
    int64_t         payload_len() const { return payload_len_; }

    // Relinquish ownership of the mint ref to the caller (the buffer is NOT
    // released on drop afterward). Used by the C door's blob_mint, which hands
    // the raw handle to a C caller who owns it until adopt+seal. Returns the
    // handle and nulls this BufRef.
    xi_image_handle take() {
        xi_image_handle h = handle_;
        handle_ = XI_IMAGE_NULL; payload_ = nullptr; payload_len_ = 0;
        return h;
    }

private:
    void reset_() {
        if (handle_) pack_pool::release(handle_);
        handle_ = XI_IMAGE_NULL; payload_ = nullptr; payload_len_ = 0;
    }
    void move_(BufRef&& o) {
        handle_ = o.handle_; payload_ = o.payload_; payload_len_ = o.payload_len_;
        o.handle_ = XI_IMAGE_NULL; o.payload_ = nullptr; o.payload_len_ = 0;
    }
    xi_image_handle handle_ = XI_IMAGE_NULL;
    uint8_t*        payload_ = nullptr;
    int64_t         payload_len_ = 0;
};

// Mint a self-describing pool buffer: write the head (magic + desc_len + the
// canonical descriptor + zero pad) and expose the 64B-aligned payload region for
// in-place fill. The descriptor is validated fail-loud (canonical map); a bad
// descriptor, a negative length, or an over-cap total returns an empty BufRef.
// The payload region is zero-filled canvas (alloc_canvas) — a producer that
// writes only part of it leaves the rest honestly zero.
inline BufRef mint_blob(const void* desc, int32_t desc_len, int64_t payload_len) {
    if (!desc || desc_len <= 0 || payload_len < 0) return {};
    const uint8_t* d = static_cast<const uint8_t*>(desc);
    if (!blob_desc_is_canonical_map(d, uint32_t(desc_len))) return {};
    const uint64_t payload_off = blob_payload_off(uint32_t(desc_len));
    const uint64_t total = payload_off + uint64_t(payload_len);
    if (total > uint64_t(INT32_MAX)) return {};                    // pool per-buffer cap
    xi_image_handle h = pack_pool::alloc_canvas(size_t(total));
    if (!h) return {};
    uint8_t* base = pack_pool::writable(h);
    pack_mp_detail::put_u32_le(base + 0, kBlobMagic);
    pack_mp_detail::put_u32_le(base + 4, uint32_t(desc_len));
    std::memcpy(base + 8, d, size_t(desc_len));
    // The pad [8+desc_len, payload_off) is already zero (alloc_canvas).
    return BufRef{h, base + payload_off, payload_len};
}

// A borrowed const view of a Blob entry: the descriptor (canonical msgpack map)
// and the 64B-aligned payload span, both zero-copy over the pool buffer, valid
// for the owning Pack's life. `handle` is the backing buffer, BORROWED (not
// addref'd) — pair with adopt_blob (which addrefs) to carry it into another pack.
struct BlobView {
    std::span<const uint8_t> desc;      // canonical msgpack descriptor map
    std::span<const uint8_t> payload;   // 64B-aligned payload bytes
    int64_t         payload_len = 0;
    xi_image_handle handle = XI_IMAGE_NULL;
};

// ---- descriptor convenience (CONVENTION layer; the core owns no type space) --
// A small writer that produces a canonical descriptor map from a type string +
// ordered key/value pairs. The SDK image helper sits on this. "t" is a
// convention (put it first); the core neither requires nor interprets it.
class BlobDesc {
public:
    // Start a descriptor and stamp the convention type key "t" first.
    explicit BlobDesc(std::string_view type) { str("t", type); }
    BlobDesc() = default;

    BlobDesc& str(std::string_view key, std::string_view v) {
        kvs_.push_back(KV{std::string(key), Kind::Str, 0, std::string(v)});
        return *this;
    }
    BlobDesc& i64(std::string_view key, int64_t v) {
        kvs_.push_back(KV{std::string(key), Kind::I64, v, {}});
        return *this;
    }
    // Emit the canonical msgpack map bytes.
    mp::Bytes build() const {
        mp::Writer w;
        w.map(uint32_t(kvs_.size()));
        for (const KV& kv : kvs_) {
            w.key(kv.key);
            if (kv.kind == Kind::Str) w.str(kv.s);
            else                      w.int_(kv.i);
        }
        return w.take();
    }

private:
    enum class Kind { Str, I64 };
    struct KV { std::string key; Kind kind; int64_t i; std::string s; };
    std::vector<KV> kvs_;
};

// Convention: element byte size for the xi/image "dt" strings. Unknown -> 0.
inline int64_t image_dtype_elem_size(std::string_view dt) {
    if (dt == "u8")  return 1;
    if (dt == "u16") return 2;
    if (dt == "i32") return 4;
    if (dt == "f32") return 4;
    if (dt == "f64") return 8;
    return 0;
}

// Build an xi/image descriptor: {"t":"xi/image","w","h","c","dt"}. Convention
// helper (the SDK's mint_image / as_cv read this exact shape). Returns empty on
// an unknown dtype.
inline mp::Bytes make_image_desc(int32_t w, int32_t h, int32_t c,
                                 std::string_view dt = "u8") {
    if (image_dtype_elem_size(dt) == 0) return {};
    return BlobDesc("xi/image")
        .i64("w", w).i64("h", h).i64("c", c).str("dt", dt)
        .build();
}

// Mint an xi/image blob: build the descriptor + mint_blob with payload sized
// w*h*c*elem_size(dt). The convenience the SDK image producer sits on. Empty
// BufRef on a bad shape / unknown dtype.
inline BufRef mint_image(int32_t w, int32_t h, int32_t c, std::string_view dt = "u8") {
    if (w <= 0 || h <= 0 || c <= 0) return {};
    const int64_t elem = image_dtype_elem_size(dt);
    if (elem == 0) return {};
    mp::Bytes desc = make_image_desc(w, h, c, dt);
    if (desc.empty()) return {};
    return mint_blob(desc.data(), int32_t(desc.size()),
                     int64_t(w) * h * c * elem);
}

namespace pack_detail {

// ---- slab layout structs (POD, live inside the slab) ----------------------
inline constexpr uint32_t kPackMagic   = 0x334B5058u;  // 'XPK3' little-endian
inline constexpr uint32_t kPackVersion = 1;

inline constexpr uint8_t kStorageInline = 0;
inline constexpr uint8_t kStorageExtern = 1;

struct PackHeader {                  // 64 bytes exactly
    uint32_t magic;                  // 'XPK3'
    uint32_t version;
    uint32_t entry_count;
    uint32_t ext_count;              // entries holding a LIVE pool handle
    uint32_t dir_offset;             // == 64
    uint32_t order_offset;           // == 64 + 32*n (the ordinal->dir table)
    uint32_t payload_offset;         // 8-aligned
    uint32_t flags;                  // 0
    uint64_t slab_bytes;             // logical slab size (<= SlabBuf cap)
    uint64_t pack_id;                // monotonic mint counter (diagnostics)
    int64_t  ts_us;                  // 0 unless a caller stamps it at seal
    uint8_t  pad[8];
};
static_assert(sizeof(PackHeader) == 64, "PackHeader must be 64 bytes");

// One directory row. Sorted by (key_hash, key bytes, ordinal) so lookup is a
// binary search and duplicate keys keep first-inserted-wins semantics.
struct DirEntry {                    // 32 bytes exactly
    uint64_t key_hash;
    uint32_t key_off;                // slab-relative offset of the key bytes
    uint32_t key_len;
    uint32_t ordinal;                // insertion index (0-based)
    uint32_t off;                    // payload: inline bytes, or the ExtRecord
    uint32_t len;                    // inline byte length / sizeof(ExtRecord)
    uint8_t  tag;                    // PackTag
    uint8_t  storage;                // kStorageInline | kStorageExtern
    uint16_t reserved;               // reserved-zero (was type_id; dtype/blob-type retired)
};
static_assert(sizeof(DirEntry) == 32, "DirEntry must be 32 bytes");

// The EXTERN side-record, stored 8-aligned in the payload: the pool handle + the
// entry's total byte length (a Bin's byte length, or a Blob's WHOLE
// self-describing buffer length). The logical shape retired with images/tensors
// — a Blob's shape lives in its descriptor, not the core struct.
struct ExtRecord {                   // 16 bytes
    xi_image_handle handle;          // XI_IMAGE_NULL if the mint failed
    uint64_t total_len;              // logical byte length (Bin) / whole buffer (Blob)
};
static_assert(sizeof(ExtRecord) == 16, "ExtRecord must be 16 bytes");

// ---- builder staging -------------------------------------------------------
struct TmpEntry {
    uint64_t hash;
    uint32_t key_off, key_len;       // payload-relative (rebased at seal)
    uint32_t off, len;               // payload-relative
    uint8_t  tag, storage;
    xi_image_handle handle;          // EXTERN: the ref this builder owns
};

// Staging (payload bytes + entry rows + the seal-time sort permutation) lives in
// a per-thread SCRATCH recycle, so a steady stream of builds on one thread is
// heap-free after warmup — the same discipline SlabPool gives the sealed slabs.
// (③ doc 28: sort_idx recycles HERE instead of a per-PackBuilder member that
// malloc'd/freed on every seal().)
struct BuilderScratch {
    std::vector<uint8_t>  payload;
    std::vector<TmpEntry> entries;
    std::vector<uint32_t> sort_idx;                      // seal-time permutation
    void clear() { payload.clear(); entries.clear(); sort_idx.clear(); }  // capacity kept
};

// ONE pool, shared by get and put — two separate thread_locals here would
// silently defeat the recycle. Hidden behind a trivially-destructible
// thread_local POINTER for the same teardown reason as SlabPool above.
using ScratchFreelist = std::vector<std::unique_ptr<BuilderScratch>>;
inline ScratchFreelist* tls_scratch_pool() {
    thread_local ScratchFreelist* slot = nullptr;
    struct Owner {
        ScratchFreelist** s;
        explicit Owner(ScratchFreelist** slot_) : s(slot_) { *s = new ScratchFreelist(); }
        ~Owner() { delete *s; *s = nullptr; }
    };
    thread_local Owner owner{&slot};
    return slot;
}
inline BuilderScratch* scratch_get() {
    ScratchFreelist* pool = tls_scratch_pool();
    if (pool && !pool->empty()) {
        BuilderScratch* s = pool->back().release();
        pool->pop_back();
        return s;
    }
    return new BuilderScratch();
}
inline void scratch_put(BuilderScratch* s) {
    s->clear();
    ScratchFreelist* pool = tls_scratch_pool();
    if (pool && pool->size() < 8) pool->emplace_back(s);
    else delete s;
}

} // namespace pack_detail

class PackBuilder;

// ===================================================================
// Pack — a sealed, immutable, single-owner keyed buffer over one slab.
//
// Only produced by PackBuilder::seal(). Move-only: exactly one owner at a time,
// whose destruction is the whole lifecycle end — directory walked once to
// release every pool handle, slab returned to the per-thread recycle pool. A
// moved-from Pack owns nothing and releases nothing.
//
// LOOKUP: binary search on the hash-sorted directory (equal-hash runs
// memcmp-verified, first-inserted wins on duplicate keys). ITERATION
// (key_at/tag_at/for_each/for_each_entry) is INSERTION order via the order table.
// ===================================================================
class Pack {
public:
    Pack() = default;   // empty/null pack (default-constructed, owns nothing)
    Pack(Pack&& o) noexcept { move_from(std::move(o)); }
    Pack& operator=(Pack&& o) noexcept {
        if (this != &o) { destroy(); move_from(std::move(o)); }
        return *this;
    }
    Pack(const Pack&) = delete;
    Pack& operator=(const Pack&) = delete;
    ~Pack() { destroy(); }

    // ---- structure ---------------------------------------------------
    size_t size()  const { return slab_.data ? header()->entry_count : 0; }
    bool   empty() const { return size() == 0; }
    bool   has(std::string_view key) const { return find(key) != nullptr; }

    std::optional<PackTag> tag_of(std::string_view key) const {
        const auto* e = find(key);
        return e ? std::optional<PackTag>(PackTag(e->tag)) : std::nullopt;
    }

    // Insertion-ordered key walk — the generic-plugin path (record_save,
    // expose): visit every entry without knowing its producer.
    template <class Fn>
    void for_each(Fn&& fn) const {
        const size_t n = size();
        for (size_t i = 0; i < n; ++i) {
            const pack_detail::DirEntry& e = dir_at(i);
            fn(key_of(e), PackTag(e.tag));
        }
    }

    // Insertion-ordered index accessors — the O(1) primitives the C-ABI generic
    // walk (key_at/tag_at) is built on. UB if i >= size().
    std::string_view key_at(size_t i) const { return key_of(dir_at(i)); }
    PackTag          tag_at(size_t i) const { return PackTag(dir_at(i).tag); }

    // RAW stored payload bytes of the i-th entry (insertion order). memory ==
    // wire (④A): the inline payload IS the entry's canonical msgpack value.
    // Empty for an EXTERN entry (a Bin above kPackLargeThreshold, or a Blob) —
    // resolve those with get_bin / get_blob. UB if i >= size().
    std::span<const uint8_t> raw_at(size_t i) const {
        const pack_detail::DirEntry& e = dir_at(i);
        if (e.storage != pack_detail::kStorageInline) return {};
        return std::span<const uint8_t>(slab() + e.off, e.len);
    }

    // ================= serialization walk API =========================
    // The port surface for the record/expose/ingress call sites: walk every
    // entry in INSERTION order with typed detail. No slab internals leak through.
    struct EntryView {
        size_t           ordinal = 0;      // insertion index
        std::string_view key;
        PackTag          tag = PackTag::I64;
        bool             external = false; // payload lives in a pool buffer
        // INLINE entries: the raw stored canonical bytes (== raw_at(ordinal)).
        std::span<const uint8_t> raw;
        // EXTERN entries: total byte length + the pool handle (borrowed, NOT
        // addref'd). A Blob's bytes ARE its self-describing buffer (validate +
        // parse the head to reach the descriptor/payload — get_blob does this).
        size_t           ext_len = 0;
        xi_image_handle  handle = XI_IMAGE_NULL;
    };

    // Visit every entry, insertion order, with an EntryView. The view's spans
    // borrow from the slab (valid while the Pack lives).
    template <class Fn>
    void for_each_entry(Fn&& fn) const {
        const size_t n = size();
        for (size_t i = 0; i < n; ++i) fn(entry_at(i));
    }
    EntryView entry_at(size_t i) const {   // UB if i >= size()
        const pack_detail::DirEntry& e = dir_at(i);
        EntryView v;
        v.ordinal  = i;
        v.key      = key_of(e);
        v.tag      = PackTag(e.tag);
        v.external = e.storage == pack_detail::kStorageExtern;
        if (v.external) {
            const pack_detail::ExtRecord& r = ext_of(e);
            v.ext_len = size_t(r.total_len);
            v.handle  = r.handle;
        } else {
            v.raw = std::span<const uint8_t>(slab() + e.off, e.len);
        }
        return v;
    }

    // Append the i-th entry's ONE canonical msgpack value to `w` — the exact wire
    // bytes for a scalar/str/bin/mp entry:
    //   I64 -> int64 0xd3        F64 -> float64 0xcb (NaN already flattened)
    //   Bool -> 0xc2/0xc3        Str -> str32 0xdb
    //   Bin (inline OR pooled) -> bin32 0xc6 over the payload bytes
    //   Mp  -> the stored canonical bytes VERBATIM
    // memory == wire (④A): for an INLINE entry the stored payload IS that
    // canonical value, so this is a verbatim splice of raw_at(i). A pooled Bin
    // re-wraps its pool bytes as bin32. A Blob has NO single canonical scalar
    // form (its wire arm is the self-describing buffer verbatim — the wire
    // package's contract): returns false, writer untouched. Also false for a
    // pooled Bin/Blob whose buffer died (never emits a poisoned value).
    bool canonical_value(size_t i, xi::mp::Writer& w) const {
        const pack_detail::DirEntry& e = dir_at(i);
        if (PackTag(e.tag) == PackTag::Blob) return false;
        if (e.storage == pack_detail::kStorageInline) {
            w.raw_canonical(slab() + e.off, e.len);   // inline payload IS canonical
            return true;
        }
        // EXTERN Bin: the pool bytes become a canonical bin32 on the wire.
        const pack_detail::ExtRecord& r = ext_of(e);
        auto v = pack_pool::view(r.handle);
        if (!r.handle || v.size() < r.total_len) return false;   // F1 guard
        w.bin(v.data(), size_t(r.total_len));
        return true;
    }

    // ---- typed borrowed reads (skip the canonical msgpack header) ------
    std::optional<int64_t> get_i64(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::I64) return std::nullopt;
        return pack_mp_detail::read_i64(slab() + e->off);
    }
    std::optional<double> get_f64(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::F64) return std::nullopt;
        return pack_mp_detail::read_f64(slab() + e->off);
    }
    std::optional<bool> get_bool(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Bool) return std::nullopt;
        return pack_mp_detail::read_bool(slab() + e->off);
    }
    std::optional<std::string_view> get_str(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Str) return std::nullopt;
        return pack_mp_detail::read_str(slab() + e->off);
    }
    // Binary: resolves storage duality — inline slab bytes OR a pool buffer,
    // both surfaced as one const span (D1 "storage duality, API unity").
    std::optional<std::span<const uint8_t>> get_bin(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Bin) return std::nullopt;
        if (e->storage == pack_detail::kStorageExtern) {
            // Guard the pooled span (F1): a null handle (pool exhaustion at
            // build) or an under-sized pool view must report absence.
            const pack_detail::ExtRecord& r = ext_of(*e);
            auto v = pack_pool::view(r.handle);
            if (!r.handle || v.size() < r.total_len) return std::nullopt;
            return v.first(size_t(r.total_len));
        }
        // INLINE: the payload is a canonical bin32 — skip its header (④A).
        return pack_mp_detail::read_bin(slab() + e->off);
    }
    // Self-describing blob: validate the head, return the descriptor map view +
    // the 64B-aligned payload span (both zero-copy over the pool buffer). nullopt
    // on absent/wrong-tag key, a dead/under-sized pool buffer (F1 guard), or a
    // buffer that fails blob_head_validate (defence in depth — adopt validated,
    // but a Blob entry always re-derives its offsets from a validated head).
    std::optional<BlobView> get_blob(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Blob) return std::nullopt;
        const pack_detail::ExtRecord& r = ext_of(*e);
        auto buf = pack_pool::view(r.handle);
        if (!r.handle || buf.size() < r.total_len) return std::nullopt;   // F1 guard
        if (!blob_head_validate(buf.data(), size_t(r.total_len))) return std::nullopt;
        const uint32_t desc_len = pack_mp_detail::get_u32_le(buf.data() + 4);
        const uint64_t payload_off = blob_payload_off(desc_len);
        BlobView v;
        v.desc        = buf.subspan(8, desc_len);
        v.payload     = buf.subspan(size_t(payload_off), size_t(r.total_len) - size_t(payload_off));
        v.payload_len = int64_t(r.total_len) - int64_t(payload_off);
        v.handle      = r.handle;
        return v;
    }
    // Convenience SUGAR: the blob's convention type string "t". nullopt if the
    // key is absent / not a blob / the descriptor has no string "t". The core
    // reads only this one convention key, and only here.
    std::optional<std::string_view> type_of(std::string_view key) const {
        auto b = get_blob(key);
        if (!b) return std::nullopt;
        return desc_find_str(b->desc, "t");
    }
    // Opaque nested msgpack pass-through (unknown type tags, arrays, maps).
    std::optional<std::span<const uint8_t>> get_mp(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Mp) return std::nullopt;
        return std::span<const uint8_t>(slab() + e->off, e->len);
    }

    // Doc-flavored get<i64>/get<f64> aliases (the _keys.h accessor style).
    template <class T> std::optional<T> get(std::string_view key) const;

    // ---- diagnostics / ABI hooks ---------------------------------------
    size_t slab_bytes()   const { return slab_.data ? size_t(header()->slab_bytes) : 0; }
    size_t handle_count() const { return slab_.data ? header()->ext_count : 0; }
    const uint8_t* slab_data() const { return slab_.data.get(); }

    // Read a string value for `key` from a canonical descriptor map (string keys,
    // fixed-width forms). Convention-layer helper (type_of sits on it; exported
    // so the SDK image accessors can read w/h/c/dt the same way). nullopt if the
    // key is absent or its value is not a string.
    static std::optional<std::string_view> desc_find_str(std::span<const uint8_t> desc,
                                                         std::string_view key) {
        mp::Reader r(desc.data(), desc.size());
        mp::Element m;
        if (r.next(m) != mp::Status::Ok || m.kind != mp::Kind::Map) return std::nullopt;
        for (uint32_t i = 0; i < m.len; ++i) {
            mp::Element k, v;
            if (r.next(k) != mp::Status::Ok || k.kind != mp::Kind::Str) return std::nullopt;
            std::string_view kk(reinterpret_cast<const char*>(k.data), k.len);
            if (r.next(v) != mp::Status::Ok) return std::nullopt;
            if (kk == key) {
                if (v.kind != mp::Kind::Str) return std::nullopt;
                return std::string_view(reinterpret_cast<const char*>(v.data), v.len);
            }
            skip_value(r, v);
        }
        return std::nullopt;
    }
    // Read an int value for `key` from a canonical descriptor map. Convention
    // helper (SDK reads "w"/"h"/"c"). nullopt if absent or non-integer.
    static std::optional<int64_t> desc_find_i64(std::span<const uint8_t> desc,
                                               std::string_view key) {
        mp::Reader r(desc.data(), desc.size());
        mp::Element m;
        if (r.next(m) != mp::Status::Ok || m.kind != mp::Kind::Map) return std::nullopt;
        for (uint32_t i = 0; i < m.len; ++i) {
            mp::Element k, v;
            if (r.next(k) != mp::Status::Ok || k.kind != mp::Kind::Str) return std::nullopt;
            std::string_view kk(reinterpret_cast<const char*>(k.data), k.len);
            if (r.next(v) != mp::Status::Ok) return std::nullopt;
            if (kk == key) {
                if (v.kind == mp::Kind::Int)  return v.i;
                if (v.kind == mp::Kind::UInt) return int64_t(v.u);
                return std::nullopt;
            }
            skip_value(r, v);
        }
        return std::nullopt;
    }

private:
    friend class PackBuilder;

    Pack(pack_detail::SlabBuf&& slab) noexcept : slab_(std::move(slab)) {}

    const uint8_t* slab() const { return slab_.data.get(); }
    const pack_detail::PackHeader* header() const {
        return reinterpret_cast<const pack_detail::PackHeader*>(slab());
    }
    const pack_detail::DirEntry* dir() const {
        return reinterpret_cast<const pack_detail::DirEntry*>(slab() + header()->dir_offset);
    }
    const uint32_t* order() const {
        return reinterpret_cast<const uint32_t*>(slab() + header()->order_offset);
    }
    const pack_detail::DirEntry& dir_at(size_t ordinal) const {
        return dir()[order()[ordinal]];
    }
    std::string_view key_of(const pack_detail::DirEntry& e) const {
        return std::string_view(reinterpret_cast<const char*>(slab() + e.key_off),
                                e.key_len);
    }
    const pack_detail::ExtRecord& ext_of(const pack_detail::DirEntry& e) const {
        return *reinterpret_cast<const pack_detail::ExtRecord*>(slab() + e.off);
    }

    // Advance a Reader past the children of an already-read container element
    // `v` (a descriptor value that is a nested map/array). Scalars/str/bin have
    // already consumed their payload in next(); only container children remain.
    static void skip_value(mp::Reader& r, const mp::Element& v) {
        uint64_t remaining = 0;
        if (v.kind == mp::Kind::Array) remaining = v.len;
        else if (v.kind == mp::Kind::Map) remaining = uint64_t(v.len) * 2;
        while (remaining--) {
            mp::Element c;
            if (r.next(c) != mp::Status::Ok) return;
            skip_value(r, c);
        }
    }

    const pack_detail::DirEntry* find(std::string_view key) const {
        if (!slab_.data) return nullptr;
        const pack_detail::DirEntry* d = dir();
        const uint32_t n = header()->entry_count;
        const uint64_t h = pack_detail::hash_key(key);
        uint32_t lo = 0, hi = n;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (d[mid].key_hash < h) lo = mid + 1; else hi = mid;
        }
        for (; lo < n && d[lo].key_hash == h; ++lo) {
            if (d[lo].key_len == key.size() &&
                std::memcmp(slab() + d[lo].key_off, key.data(), key.size()) == 0)
                return &d[lo];
        }
        return nullptr;
    }

    void destroy() {
        if (!slab_.data) return;
        const pack_detail::DirEntry* d = dir();
        const uint32_t n = header()->entry_count;
        for (uint32_t i = 0; i < n; ++i)
            if (d[i].storage == pack_detail::kStorageExtern)
                pack_pool::release(ext_of(d[i]).handle);   // release(0) is a no-op
        pack_detail::slab_release(std::move(slab_));
        slab_ = {};
    }
    void move_from(Pack&& o) noexcept {
        slab_ = std::move(o.slab_);
        o.slab_ = {};
    }

    pack_detail::SlabBuf slab_;
};

template <> inline std::optional<int64_t> Pack::get<int64_t>(std::string_view k) const { return get_i64(k); }
template <> inline std::optional<double>  Pack::get<double>(std::string_view k) const  { return get_f64(k); }
template <> inline std::optional<bool>    Pack::get<bool>(std::string_view k) const    { return get_bool(k); }

// ===================================================================
// PackBuilder — the pre-seal, insertion-ordered entry table.
//
// The ONLY way to populate a pack. add_* assert the builder is not yet sealed.
// Staging lives in the per-thread scratch recycle; seal() sorts the directory,
// writes the slab in one pass, and hands it to an immutable Pack. A builder
// abandoned without seal() releases every pool handle it minted or adopted.
// ===================================================================
class PackBuilder {
public:
    PackBuilder() : s_(pack_detail::scratch_get()) {}
    PackBuilder(PackBuilder&& o) noexcept : s_(o.s_), sealed_(o.sealed_) {
        o.s_ = nullptr;
        o.sealed_ = true;
    }
    PackBuilder& operator=(PackBuilder&& o) noexcept {
        if (this != &o) {
            abandon_();
            s_ = o.s_; sealed_ = o.sealed_;
            o.s_ = nullptr; o.sealed_ = true;
        }
        return *this;
    }
    PackBuilder(const PackBuilder&) = delete;
    PackBuilder& operator=(const PackBuilder&) = delete;
    ~PackBuilder() { abandon_(); }

    bool sealed() const { return sealed_; }

    // memory == wire (④A): the inline payload IS the entry's canonical msgpack
    // value, encoded here at add-time through the ONE canonical truth.
    void add_i64(std::string_view key, int64_t v) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::I64);
        e.off = bump_(uint32_t(pack_mp_detail::kI64Size), 1);
        e.len = uint32_t(pack_mp_detail::kI64Size);
        pack_mp_detail::write_i64(s_->payload.data() + e.off, v);
        s_->entries.push_back(e);
    }
    void add_f64(std::string_view key, double v) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::F64);
        e.off = bump_(uint32_t(pack_mp_detail::kF64Size), 1);
        e.len = uint32_t(pack_mp_detail::kF64Size);
        pack_mp_detail::write_f64(s_->payload.data() + e.off, v);
        s_->entries.push_back(e);
    }
    void add_bool(std::string_view key, bool v) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::Bool);
        e.off = bump_(uint32_t(pack_mp_detail::kBoolSize), 1);
        e.len = uint32_t(pack_mp_detail::kBoolSize);
        pack_mp_detail::write_bool(s_->payload.data() + e.off, v);
        s_->entries.push_back(e);
    }
    void add_str(std::string_view key, std::string_view v) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::Str);
        const size_t enc = pack_mp_detail::str_size(v.size());
        e.off = bump_(uint32_t(enc), 1);
        e.len = uint32_t(enc);
        pack_mp_detail::write_str(s_->payload.data() + e.off, v);
        s_->entries.push_back(e);
    }
    // Binary: small stays inline (raw canonical bin32 in the slab); large is
    // minted into a pool buffer (D1). Either way get_bin returns one span.
    void add_bin(std::string_view key, const void* data, size_t n) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return;
        if (n >= kPackLargeThreshold && data) {
            xi_image_handle h = pack_pool::alloc_bytes(data, n);
            if (h) {
                push_extern_(key, PackTag::Bin, h, n);
                return;
            }
            // Pool exhausted / alloc failed. NEVER store a live-looking extern
            // entry with a null handle (the F1 bug). Fall back to INLINE slab
            // storage so the bytes still ride, honestly (no silent data loss).
            std::fprintf(stderr,
                "[xinsp2] pack add_bin('%.*s'): pool alloc failed for %zu bytes; "
                "storing inline\n",
                int(key.size()), key.data(), n);
        }
        // INLINE bin: store the canonical bin32 value (memory == wire, ④A).
        pack_detail::TmpEntry e = begin_(key, PackTag::Bin);
        const size_t enc = pack_mp_detail::bin_size(n);
        e.off = bump_(uint32_t(enc), 1);
        e.len = uint32_t(enc);
        pack_mp_detail::write_bin(s_->payload.data() + e.off, data, n);
        s_->entries.push_back(e);
    }

    // ---- self-describing blobs (spec 30) -------------------------------
    // Adopt an ALREADY-minted, self-describing pool buffer as a Blob entry
    // (zero-copy). Validates the head fail-loud (blob_head_validate over the
    // whole buffer) and addrefs so the pack co-owns it; the caller keeps its own
    // ref (a BufRef releases it on drop). Fails closed (false, nothing added, no
    // addref) on a null/dead handle or a buffer that is not a valid blob.
    bool adopt_blob(std::string_view key, xi_image_handle handle) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return false;
        if (!handle) return false;
        auto v = pack_pool::view(handle);
        if (!blob_head_validate(v.data(), v.size())) return false;
        pack_pool::addref(handle);
        push_extern_(key, PackTag::Blob, handle, v.size());
        return true;
    }
    // Ergonomic overload: adopt a freshly minted BufRef (the buffer whose head
    // mint_blob wrote). addref (pack co-owns rc); the BufRef still releases its
    // OWN mint ref on drop, so the pack holds it alone afterward.
    bool adopt_blob(std::string_view key, const BufRef& ref) {
        return adopt_blob(key, ref.handle());
    }
    // Convenience: mint a self-describing buffer, copy `payload` into its aligned
    // region, and adopt it — the copy path (foreign bytes / a producer that has
    // the payload in hand). Returns false on an invalid descriptor, a bad length,
    // or pool exhaustion (nothing added). The pack owns the buffer alone after.
    bool add_blob(std::string_view key, const void* desc, int32_t desc_len,
                  const void* payload, int64_t payload_len) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return false;
        BufRef ref = mint_blob(desc, desc_len, payload_len);
        if (!ref) return false;
        if (payload && payload_len > 0)
            std::memcpy(ref.payload(), payload, size_t(payload_len));
        return adopt_blob(key, ref);   // addref rc2; ref drop -> rc1; pack holds rc1
    }

    // Opaque nested msgpack (already canonical): copied verbatim into the slab.
    //
    // GUARD: this is the path for INTERNAL producers whose bytes are canonical
    // BY CONSTRUCTION. It does NOT validate — it trusts. FOREIGN / untrusted
    // bytes must go through xi::ingress::canonicalize_entry / canonicalize_into
    // (xi_ingress.hpp), which validates, normalizes, and refuses forged
    // pool-handle ext BEFORE producing the canonical bytes this method stores.
    void add_mp(std::string_view key, const void* mp, size_t n) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::Mp);
        e.off = bump_(uint32_t(n), 8);
        e.len = uint32_t(n);
        if (n) std::memcpy(s_->payload.data() + e.off, mp, n);
        s_->entries.push_back(e);
    }

    // Flip immutable: sort the directory, write the slab in one pass, hand it to
    // a Pack. The builder is spent afterwards (scratch recycled).
    Pack seal(int64_t ts_us = 0) {
        assert(!sealed_ && "double seal");
        if (spent_()) return Pack{};
        sealed_ = true;
        auto& sc = *s_;
        const uint32_t n = uint32_t(sc.entries.size());
        const uint32_t dir_off     = uint32_t(sizeof(pack_detail::PackHeader));
        const uint32_t order_off   = dir_off + n * uint32_t(sizeof(pack_detail::DirEntry));
        const uint32_t payload_off = (order_off + n * 4u + 7u) & ~7u;   // 8-aligned
        const uint64_t slab_bytes  = uint64_t(payload_off) + sc.payload.size();

        // uint32 seal-size guard (round-1 doc 28): DirEntry off/len and
        // payload_off are uint32; a slab past 4 GiB would silently truncate every
        // offset. Fail LOUD at seal rather than mint a corrupt pack. (Inline Mp
        // has no large-threshold, so a pathological nested Mp is the one way
        // here; large bins/blobs already went EXTERN.)
        if (slab_bytes > uint64_t(UINT32_MAX)) {
            std::fprintf(stderr,
                "[xinsp2] pack seal REFUSED: slab %llu bytes exceeds the uint32 "
                "offset limit (%u entries) — returning an empty pack rather than "
                "truncating offsets.\n",
                (unsigned long long)slab_bytes, n);
            abandon_();          // release every minted/adopted handle — no leak
            return Pack{};
        }

        pack_detail::SlabBuf slab =
            pack_detail::slab_acquire(size_t(slab_bytes), kDefaultSlab);
        uint8_t* base = slab.data.get();

        // Directory order: (key_hash, key bytes, ordinal). The sort permutes an
        // index array (sc.sort_idx, recycled with the scratch — ③); TmpEntry
        // order in the scratch stays insertion order (their index IS the ordinal).
        std::vector<uint32_t>& sort_idx = sc.sort_idx;
        sort_idx.resize(n);
        for (uint32_t i = 0; i < n; ++i) sort_idx[i] = i;
        const uint8_t* pay = sc.payload.data();
        const pack_detail::TmpEntry* te = sc.entries.data();
        std::sort(sort_idx.begin(), sort_idx.end(),
                  [pay, te](uint32_t ia, uint32_t ib) {
                      const auto& a = te[ia]; const auto& b = te[ib];
                      if (a.hash != b.hash) return a.hash < b.hash;
                      const uint32_t m = a.key_len < b.key_len ? a.key_len : b.key_len;
                      int c = m ? std::memcmp(pay + a.key_off, pay + b.key_off, m) : 0;
                      if (c != 0) return c < 0;
                      if (a.key_len != b.key_len) return a.key_len < b.key_len;
                      return ia < ib;                    // stable: ordinal
                  });

        pack_detail::PackHeader hd{};
        hd.magic          = pack_detail::kPackMagic;
        hd.version        = pack_detail::kPackVersion;
        hd.entry_count    = n;
        hd.ext_count      = ext_live_;
        hd.dir_offset     = dir_off;
        hd.order_offset   = order_off;
        hd.payload_offset = payload_off;
        hd.flags          = 0;
        hd.slab_bytes     = slab_bytes;
        hd.pack_id        = next_pack_id_().fetch_add(1, std::memory_order_relaxed);
        hd.ts_us          = ts_us;
        std::memcpy(base, &hd, sizeof hd);

        auto* dir   = reinterpret_cast<pack_detail::DirEntry*>(base + dir_off);
        auto* order = reinterpret_cast<uint32_t*>(base + order_off);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t ord = sort_idx[i];
            const pack_detail::TmpEntry& t = te[ord];
            pack_detail::DirEntry d{};
            d.key_hash = t.hash;
            d.key_off  = t.key_off + payload_off;      // rebase payload -> slab
            d.key_len  = t.key_len;
            d.ordinal  = ord;
            d.off      = t.off + payload_off;
            d.len      = t.len;
            d.tag      = t.tag;
            d.storage  = t.storage;
            d.reserved = 0;
            std::memcpy(&dir[i], &d, sizeof d);
            order[ord] = i;
        }
        if (!sc.payload.empty())
            std::memcpy(base + payload_off, sc.payload.data(), sc.payload.size());

        // The pool handles now belong to the pack (its destroy walks the dir).
        sc.entries.clear();
        pack_detail::scratch_put(s_);
        s_ = nullptr;
        ext_live_ = 0;
        return Pack(std::move(slab));
    }

private:
    static constexpr size_t kDefaultSlab = 4096;

    static std::atomic<uint64_t>& next_pack_id_() {
        static std::atomic<uint64_t> id{1};
        return id;
    }

    bool spent_() const { return sealed_ || !s_; }

    pack_detail::TmpEntry begin_(std::string_view key, PackTag tag) {
        assert(!sealed_ && "add after seal");
        pack_detail::TmpEntry e{};
        e.hash    = pack_detail::hash_key(key);
        e.key_off = bump_(uint32_t(key.size()), 1);
        e.key_len = uint32_t(key.size());
        if (!key.empty())
            std::memcpy(s_->payload.data() + e.key_off, key.data(), key.size());
        e.tag     = uint8_t(tag);
        e.storage = pack_detail::kStorageInline;
        e.handle  = XI_IMAGE_NULL;
        return e;
    }
    // Register an EXTERN entry: its ExtRecord {handle, total_len} goes into the
    // payload now (the slab copy at seal carries it verbatim); the handle also
    // rides the TmpEntry so an abandoned builder can release it.
    void push_extern_(std::string_view key, PackTag tag,
                      xi_image_handle handle, uint64_t total_len) {
        pack_detail::TmpEntry e = begin_(key, tag);
        e.storage = pack_detail::kStorageExtern;
        e.handle  = handle;
        e.off     = bump_(uint32_t(sizeof(pack_detail::ExtRecord)), 8);
        e.len     = uint32_t(sizeof(pack_detail::ExtRecord));
        pack_detail::ExtRecord r{handle, total_len};
        std::memcpy(s_->payload.data() + e.off, &r, sizeof r);
        if (handle) ++ext_live_;
        s_->entries.push_back(e);
    }
    uint32_t bump_(uint32_t n, uint32_t align) {
        size_t off = (s_->payload.size() + (align - 1)) & ~size_t(align - 1);
        s_->payload.resize(off + n);
        return uint32_t(off);
    }
    void abandon_() {
        if (!s_) return;
        for (const auto& e : s_->entries)
            if (e.storage == pack_detail::kStorageExtern)
                pack_pool::release(e.handle);
        pack_detail::scratch_put(s_);
        s_ = nullptr;
        ext_live_ = 0;
    }

    pack_detail::BuilderScratch* s_ = nullptr;
    uint32_t ext_live_ = 0;            // live (non-null) handles this builder owns
    bool sealed_ = false;
};

} // namespace xi
