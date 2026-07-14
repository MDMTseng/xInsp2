#pragma once
//
// xi_pack.hpp — the v3 uniform keyed-buffer pack container, SLAB representation
// (pack-v3 migration; design C, validated by backend/tests/proto/xi_pack_c.hpp
// + bench_pack_c.cpp — the proto stays as the experiment record, out of include/;
// see docs/new_gen/29-pack-c-prototype-record.md).
//
// A pack is ONE thing:  key(string) -> entry, where an entry is
// (type tag, const-span bytes). There is no image/metadata split — an image
// is simply an entry whose tag says "image" and whose pixels live in a pool
// buffer. See docs/new_gen/07-uniform-keyed-buffer-plane.md for the decision.
//
// REPRESENTATION (what changed from the Arena+msgpack-entry container):
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
//     the contract the expose/record walkers depend on. Hash order is an
//     internal detail nothing outside find() sees.
//   * INLINE entries store scalars RAW (i64/f64 = 8 aligned bytes: a read is
//     one pointer add + one 8-byte load, zero msgpack decode; bool = 1 byte
//     0/1), strings/small bins as raw bytes (length lives in the DirEntry),
//     and nested msgpack (Mp) as its canonical bytes verbatim.
//   * EXTERN entries (Image, Tensor, Bin >= kPackLargeThreshold) live in the
//     production ImagePool exactly as before — owner-neutral mint through
//     pack_pool below, pack co-owns via the pool refcount, frozen image ABI
//     untouched. The slab payload holds a 24-byte ExtRecord {handle, logical
//     shape w/h/c, byte length} per EXTERN entry; the DirEntry points at it.
//   * The slab buffer RECYCLES through the same per-thread freelist the old
//     arena chunks used (pack_detail::SlabPool — the previous ArenaPool with
//     the honest name), and the builder's staging vectors recycle through a
//     per-thread scratch pool, so a steady stream of packs on one lane's
//     thread is heap-free after warmup — the ImagePool discipline preserved.
//
// WIRE / AT-REST FORMAT: UNCHANGED. The canonical msgpack container (doc 07
// max-width profile via xi_mp.hpp) remains the serialization truth for this
// migration; scalars are raw ONLY in memory. The walk API below
// (for_each_entry / canonical_value) re-emits each entry's canonical bytes —
// through xi::mp::Writer, the ONE canonical-encode truth, so ruling 1's NaN
// flatten and the max-width markers are byte-identical to what the old arena
// stored. (Slab-verbatim wire, as the prototype's serialize() showed, is a
// follow-up candidate, NOT this migration.)
//
// SEMANTICS KEPT: Pack is move-only, sealed-immutable, single-owner; borrowed
// views (get_str/get_bin/get_mp/raw_at spans, key_at string_views) are valid
// for the life of the owning Pack — the slab is a stable heap block, so views
// survive a Pack move, exactly like the old arena chunks. Drop-on-crash is
// EXACTLY destruction: the directory walk releases each pool handle once and
// the slab returns to the recycle pool; a moved-from Pack owns nothing.
//
// API DELTAS from the arena representation (recorded for downstream agents):
//   * raw_at(i) now returns the entry's RAW stored payload (raw 8-byte scalar,
//     raw string bytes, canonical msgpack only for Mp) instead of "canonical
//     msgpack bytes for every inline entry". Memory != wire for scalars now;
//     use canonical_value(i, writer) to get the wire bytes (byte-identical to
//     what raw_at used to return).
//   * arena_bytes() is gone; slab_bytes() reports the slab footprint.
//   * NEW: first-class typed tensors — PackTag::Tensor entries carry a dtype
//     (PackDtype in DirEntry::type_id) + logical shape {w,h,c}; see
//     add_tensor / get_tensor / get_tensor_of<T>. Images stay the u8 (w,h,c)
//     entries they always were. DirEntry::type_id is also the future custom-
//     blob type space (>= kPackTypeUserBase via add_blob).
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
//   xi::mp::Writer is the ONE canonical-encode truth. Scalars now live RAW in
//   the slab, so these fixed-offset forms are no longer the pack's storage
//   encoding — they are the WIRE forms: the canonical walk (canonical_value)
//   emits through xi::mp::Writer itself, and these readers/writers remain for
//   the fixed-offset consumers (bench_pack's offset-table reads, the XEX1
//   parse/dump helpers) that decode canonical bytes at known offsets.
//
//   Fixed-width forms only (int64 0xd3, float64 0xcb, str32 0xdb, bin32 0xc6)
//   — the canonical max-width profile of doc 07. Everything is 100% standard
//   msgpack; a stock decoder reads it.
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
// zero validation cost, zero-copy views) — the C3 fast read path for callers
// that hold canonical wire bytes (offset-table reads, the XEX1 loaders).
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
// This IS the old ArenaPool with the honest post-migration name: a sealed
// pack's slab is BORROWED here at seal() and RETURNED here when the pack is
// destroyed, so a steady stream of packs built and dropped on one thread
// reuses the same handful of buffers instead of hitting the heap allocator
// per pack. Scoped THREAD-LOCAL (the per-lane cache: a pack is built, sealed,
// read, and dropped within one lane worker's thread), so the recycle needs no
// lock. A pack that legitimately outlives its producer thread still frees
// correctly — its slab simply returns to whichever thread's pool drops it,
// or, if that pool is full, frees outright. Bounded (kMaxFree) so an idle
// thread does not hoard memory.
//
// DECISION (migration ruling 7): the slab source stays this existing
// per-thread freelist rather than the prototype's magazine+overflow-shelf
// size-class allocator — the freelist already gives steady-state heap-free
// builds with best-fit reuse (cap >= need), is the smaller diff, and EXTERN
// buffers get their size-class recycling from the production ImagePool.
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
// thread_local has been destroyed. A Pack legitimately outlives its producer
// thread and may be destroyed inside a LATER thread_local destructor on the
// same thread (or during static teardown) — that late release() must NOT touch
// a SlabPool whose free_ vector was already destroyed. With the pointer null it
// falls through to a plain heap free instead (SlabBuf's unique_ptr). This is the
// same hardening ImagePool::instance() and the pixpool shelf/magazine carry;
// SlabPool is the one recycle pool a dropped Pack reaches, so it needs it too.
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
// Borrow a slab (>= need). During normal operation the pool is live; the null
// path only happens if a build were somehow attempted during teardown.
inline SlabBuf slab_acquire(size_t need, size_t default_cap) {
    if (SlabPool* p = tls_slab_pool()) return p->acquire(need, default_cap);
    size_t cap = need > default_cap ? need : default_cap;
    return SlabBuf{std::unique_ptr<uint8_t[]>(new uint8_t[cap]), cap};
}
// Return a slab for reuse. If the pool is gone (late teardown), b frees its
// slab outright as it goes out of scope — never a UAF on a dead free_ vector.
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

} // namespace pack_detail

// ===================================================================
// pack_pool — thin TYPELESS facade over ImagePool (unchanged boundary).
//
// The pool is image-shaped (create(w,h,ch)); a typeless large buffer of N
// bytes is just a (N,1,1) entry whose pixels ARE the bytes, and an image is a
// native (w,h,c) entry. Either way the handle is minted here, released here,
// and resolved to a raw const span. This is the ONLY mint path in the pack
// layer, satisfying doc 07's "handles are mintable only by the domain's own
// allocator". See docs/new_gen/09-bufferpool-audit.md for the reuse verdict.
//
// OWNER-NEUTRAL MINT (cross-plane owner-sweep data-loss fix). A buffer minted
// INTO a pack is governed by the pack alone: the pack's directory walk
// releases it exactly once on destruction, and a LEAKED pack is reclaimed by
// the PACK registry's own owner sweep (sweep_packs_for -> pack destroy ->
// handle release). Tagging it with the producing INSTANCE's ImagePool owner
// (current_owner()) was therefore redundant AND harmful: PackRegistry::retain
// bumps only the pack-registry rc, never the pool rc, so the buffer stayed at
// pool rc 1 — and the instance's image-plane sweep (ImagePool::release_all_for
// on producer teardown) freed it out from under a co-owner still holding the
// PACK. Minting owner-0 makes the image sweep skip pack buffers so the pack
// solely governs them. Standalone mints keep the instance owner and are still
// leak-swept; the ADOPT path (adopt_image: pool addref -> rc 2 -> spared+
// orphaned by the sweep) was already correct. Diagnostic-only side effect:
// stats_by_owner attributes pack buffers to owner 0.
// ===================================================================
namespace pack_pool {

// Mint a typeless buffer of n bytes and copy src into it. 0 on failure
// (n==0, over the pool's per-buffer cap, or pool exhausted).
inline xi_image_handle alloc_bytes(const void* src, size_t n) {
    if (n == 0 || n > size_t(INT32_MAX)) return XI_IMAGE_NULL;
    ImagePool::OwnerGuard neutral(0);   // owner-neutral: pack-governed lifetime
    xi_image_handle h = ImagePool::instance().create(int32_t(n), 1, 1);
    if (h && src) std::memcpy(ImagePool::instance().data(h), src, n);
    return h;
}
// Mint an image buffer (w*h*c bytes) and copy pixels in. 0 on failure.
inline xi_image_handle alloc_image(int32_t w, int32_t h, int32_t c, const void* px) {
    ImagePool::OwnerGuard neutral(0);   // owner-neutral: pack-governed lifetime
    xi_image_handle handle = ImagePool::instance().create(w, h, c);
    if (handle && px) {
        std::memcpy(ImagePool::instance().data(handle),
                    px, size_t(w) * size_t(h) * size_t(c));
    }
    return handle;
}
inline void addref(xi_image_handle h) {
    if (h) ImagePool::instance().addref(h);
}
inline void release(xi_image_handle h) {
    // Safe even for a pack destroyed during static teardown: the pool
    // singleton is intentionally leaked (ImagePool::instance), never destroyed.
    if (h) ImagePool::instance().release(h);
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
// pool-buffer reference (Bin above threshold, Image, Tensor). Unknown/opaque
// nested msgpack rides as Mp — forward compatibility by construction.
// Values are FROZEN 1:1 against XI_PACK_TAG_* in xi_abi.h; Tensor is APPENDED
// (=7) exactly as Bool (=6) was.
// ===================================================================
enum class PackTag : uint8_t { I64, F64, Str, Bin, Image, Mp, Bool, Tensor };

// Element dtype for Tensor entries — rides in DirEntry::type_id.
enum class PackDtype : uint16_t { U8 = 0, U16 = 1, I32 = 2, F32 = 3, F64 = 4 };
inline constexpr uint32_t pack_dtype_elem_size(PackDtype d) {
    constexpr uint32_t k[] = {1, 2, 4, 4, 8};
    return k[uint16_t(d)];
}
// C++ element type -> PackDtype (drives get_tensor_of<T>). Unsupported T = no
// specialization = compile error, so a bogus element type can't slip through.
template <class T> struct pack_dtype_of;                  // intentionally undefined
template <> struct pack_dtype_of<uint8_t>  { static constexpr PackDtype value = PackDtype::U8;  };
template <> struct pack_dtype_of<uint16_t> { static constexpr PackDtype value = PackDtype::U16; };
template <> struct pack_dtype_of<int32_t>  { static constexpr PackDtype value = PackDtype::I32; };
template <> struct pack_dtype_of<float>    { static constexpr PackDtype value = PackDtype::F32; };
template <> struct pack_dtype_of<double>   { static constexpr PackDtype value = PackDtype::F64; };

// DirEntry::type_id space: 0 = "the tag speaks for itself"; Tensor entries
// carry their PackDtype here; values >= kPackTypeUserBase are the caller's
// typed-blob space (add_blob) for future custom payloads.
inline constexpr uint16_t kPackTypeUserBase = 0x100;

// A borrowed const view of an image entry: dimensions + a zero-copy span over
// the pool buffer's pixels. Valid for the lifetime of the owning Pack.
struct PackImageView {
    int32_t width    = 0;
    int32_t height   = 0;
    int32_t channels = 0;
    std::span<const uint8_t> pixels;
};

// A borrowed const view of a tensor entry: logical shape {w,h,c} + dtype +
// a zero-copy span over the pool buffer's element bytes. `handle` is the
// backing pool buffer, BORROWED (not addref'd) — pair with pack_pool::addref
// (or PackBuilder::adopt_tensor, which addrefs) to keep it past the pack.
struct PackTensorView {
    int32_t  width    = 0;
    int32_t  height   = 0;
    int32_t  channels = 0;
    PackDtype dtype   = PackDtype::U8;
    uint32_t elem_size = 1;              // bytes per element
    std::span<const uint8_t> bytes;      // w*h*c*elem_size raw element bytes
    xi_image_handle handle = XI_IMAGE_NULL;  // backing pool buffer (borrowed)
};

// A borrowed const view of a user-typed blob (a Bin entry read together with
// its DirEntry::type_id): 0 = a plain bin, >= kPackTypeUserBase = the
// producer's declared payload type (add_blob / adopt_bin).
struct PackBlobView {
    uint16_t type_id = 0;
    std::span<const uint8_t> bytes;
};

// Element-typed tensor view — get_tensor_of<T>: same buffer, but `data`
// already carries the element type and `count` is in ELEMENTS.
template <class T>
struct PackTensorOf {
    const T* data = nullptr;
    size_t   count = 0;                  // elements, not bytes
    int32_t  width = 0, height = 0, channels = 0;
    bool ok() const { return data != nullptr; }
};

// Bytes at/above this size go to a pool buffer instead of the slab (D1
// storage duality). Small enough that scalars/short strings stay inline;
// large enough that kilobyte metadata does not churn the pool.
inline constexpr size_t kPackLargeThreshold = 4096;

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
    uint16_t type_id;                // 0 | PackDtype (Tensor) | user blob type
};
static_assert(sizeof(DirEntry) == 32, "DirEntry must be 32 bytes");

// The EXTERN side-record, stored 8-aligned in the payload: the pool handle +
// the entry's LOGICAL shape/length (the pool's own dims are the storage shape
// — (n,1,1) for bins/tensors — so the logical shape lives here).
struct ExtRecord {                   // 24 bytes
    xi_image_handle handle;          // XI_IMAGE_NULL if the mint failed
    int32_t  w, h, c;                // logical shape (bin: {n,1,1})
    uint32_t len;                    // logical byte length
};
static_assert(sizeof(ExtRecord) == 24, "ExtRecord must be 24 bytes");

// ---- builder staging -------------------------------------------------------
struct TmpEntry {
    uint64_t hash;
    uint32_t key_off, key_len;       // payload-relative (rebased at seal)
    uint32_t off, len;               // payload-relative
    uint8_t  tag, storage;
    uint16_t type_id;
    xi_image_handle handle;          // EXTERN: the ref this builder owns
};

// Staging (payload bytes + entry rows) lives in a per-thread SCRATCH recycle,
// so a steady stream of builds on one thread is heap-free after warmup — the
// same discipline SlabPool gives the sealed slabs.
struct BuilderScratch {
    std::vector<uint8_t>  payload;
    std::vector<TmpEntry> entries;
    void clear() { payload.clear(); entries.clear(); }   // capacity kept
};

// ONE pool, shared by get and put — two separate thread_locals here would
// silently defeat the recycle. Hidden behind a trivially-destructible
// thread_local POINTER for the same teardown reason as SlabPool above: a
// builder abandoned during static/thread teardown (scratch_put from a late
// destructor) must not touch a freelist vector that was already destroyed —
// with the pointer null it just deletes the scratch outright.
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
// Only produced by PackBuilder::seal(); there is no public constructor, so a
// pre-seal (mutable) pack can never be handed out as a Pack. Move-only:
// exactly one owner at a time, whose destruction is the whole lifecycle end —
// directory walked once to release every pool handle, slab returned to the
// per-thread recycle pool. A moved-from Pack owns nothing and releases
// nothing, so a drop can never double-release.
//
// LOOKUP: binary search on the hash-sorted directory (equal-hash runs
// memcmp-verified, first-inserted wins on duplicate keys). ITERATION
// (key_at/tag_at/for_each/for_each_entry) is INSERTION order via the slab's
// ordinal->dir order table.
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

    // Insertion-ordered index accessors — the O(1) primitives the C-ABI
    // generic walk (key_at/tag_at) is built on. UB if i >= size().
    std::string_view key_at(size_t i) const { return key_of(dir_at(i)); }
    PackTag          tag_at(size_t i) const { return PackTag(dir_at(i).tag); }
    // The DirEntry::type_id extension: 0, a PackDtype (Tensor), or a user
    // blob type (>= kPackTypeUserBase). UB if i >= size().
    uint16_t         type_id_at(size_t i) const { return dir_at(i).type_id; }

    // RAW stored payload bytes of the i-th entry (insertion order). SEMANTICS
    // CHANGED from the arena representation: scalars are the raw 8-byte value
    // (Bool: one 0/1 byte), Str/small-Bin the raw bytes, Mp the canonical
    // msgpack bytes verbatim. Empty for an EXTERN entry (Image/Tensor, or a
    // Bin above kPackLargeThreshold) — resolve those with get_image /
    // get_tensor / get_bin. For the entry's CANONICAL WIRE bytes (what raw_at
    // used to return for every inline entry) use canonical_value(i, writer).
    // UB if i >= size().
    std::span<const uint8_t> raw_at(size_t i) const {
        const pack_detail::DirEntry& e = dir_at(i);
        if (e.storage != pack_detail::kStorageInline) return {};
        return std::span<const uint8_t>(slab() + e.off, e.len);
    }

    // ================= serialization walk API =========================
    // The port surface for the record/expose/ingress call sites: walk every
    // entry in INSERTION order with full typed detail, and re-emit any
    // non-pixel entry's canonical msgpack value byte-identically to the old
    // arena bytes (max-width profile, ruling-1 NaN flatten — the emit goes
    // through xi::mp::Writer / pack_mp_detail, the one canonical truth).
    // No slab internals leak through this surface.
    struct EntryView {
        size_t           ordinal = 0;      // insertion index
        std::string_view key;
        PackTag          tag = PackTag::I64;
        uint16_t         type_id = 0;      // dtype for Tensor; user blob type
        bool             external = false; // payload lives in a pool buffer
        // INLINE entries: the raw stored bytes (== raw_at(ordinal)).
        std::span<const uint8_t> raw;
        // EXTERN entries: logical shape + length + the pool handle (borrowed,
        // NOT addref'd — pair with pack_pool::addref to keep it past the pack).
        int32_t          w = 0, h = 0, c = 0;
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
        v.type_id  = e.type_id;
        v.external = e.storage == pack_detail::kStorageExtern;
        if (v.external) {
            const pack_detail::ExtRecord& r = ext_of(e);
            v.w = r.w; v.h = r.h; v.c = r.c;
            v.ext_len = r.len;
            v.handle  = r.handle;
        } else {
            v.raw = std::span<const uint8_t>(slab() + e.off, e.len);
        }
        return v;
    }

    // Append the i-th entry's ONE canonical msgpack value to `w` — the exact
    // wire bytes today's container serialization carries for that entry:
    //   I64 -> int64 0xd3        F64 -> float64 0xcb (NaN already flattened)
    //   Bool -> 0xc2/0xc3        Str -> str32 0xdb
    //   Bin (inline OR pooled) -> bin32 0xc6 over the payload bytes
    //   Mp  -> the stored canonical bytes VERBATIM (raw_canonical splice)
    // Image/Tensor entries have NO single canonical scalar form (the wire
    // shape — descriptor map + pixel bin — is the dumper's contract, built
    // from get_image/get_tensor): returns false, writer untouched. Also false
    // for a pooled Bin whose pool buffer died (never emits a poisoned value).
    bool canonical_value(size_t i, xi::mp::Writer& w) const {
        const pack_detail::DirEntry& e = dir_at(i);
        const uint8_t* p = slab() + e.off;
        switch (PackTag(e.tag)) {
            case PackTag::I64: {
                int64_t v; std::memcpy(&v, p, 8); w.int_(v); return true;
            }
            case PackTag::F64: {
                double v; std::memcpy(&v, p, 8); w.float_(v); return true;
            }
            case PackTag::Bool: w.boolean(p[0] != 0); return true;
            case PackTag::Str:
                w.str(std::string_view(reinterpret_cast<const char*>(p), e.len));
                return true;
            case PackTag::Mp:  w.raw_canonical(p, e.len); return true;
            case PackTag::Bin: {
                if (e.storage == pack_detail::kStorageInline) {
                    w.bin(p, e.len);
                    return true;
                }
                const pack_detail::ExtRecord& r = ext_of(e);
                auto v = pack_pool::view(r.handle);
                if (!r.handle || v.size() < r.len) return false;   // F1 guard
                w.bin(v.data(), r.len);
                return true;
            }
            case PackTag::Image:
            case PackTag::Tensor:
            default:
                return false;
        }
    }

    // ---- typed borrowed reads (raw slab loads — zero decode) ----------
    std::optional<int64_t> get_i64(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::I64) return std::nullopt;
        int64_t v;
        std::memcpy(&v, slab() + e->off, 8);
        return v;
    }
    std::optional<double> get_f64(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::F64) return std::nullopt;
        double v;
        std::memcpy(&v, slab() + e->off, 8);
        return v;
    }
    std::optional<bool> get_bool(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Bool) return std::nullopt;
        return slab()[e->off] != 0;
    }
    std::optional<std::string_view> get_str(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Str) return std::nullopt;
        return std::string_view(reinterpret_cast<const char*>(slab() + e->off),
                                e->len);
    }
    // Binary: resolves storage duality — inline slab bytes OR a pool buffer,
    // both surfaced as one const span (D1 "storage duality, API unity").
    std::optional<std::span<const uint8_t>> get_bin(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Bin) return std::nullopt;
        if (e->storage == pack_detail::kStorageExtern) {
            // Guard the pooled span (F1): a null handle (pool exhaustion at
            // build) or an under-sized pool view must report absence, never a
            // poisoned {nullptr, n} span.
            const pack_detail::ExtRecord& r = ext_of(*e);
            auto v = pack_pool::view(r.handle);
            if (!r.handle || v.size() < r.len) return std::nullopt;
            return v.first(r.len);
        }
        return std::span<const uint8_t>(slab() + e->off, e->len);
    }
    // Image descriptor + zero-copy pixel span over the pool buffer.
    std::optional<PackImageView> get_image(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Image) return std::nullopt;
        const pack_detail::ExtRecord& r = ext_of(*e);
        return PackImageView{r.w, r.h, r.c, pack_pool::view(r.handle)};
    }
    // Tensor: logical shape + dtype + zero-copy element bytes.
    std::optional<PackTensorView> get_tensor(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Tensor) return std::nullopt;
        const pack_detail::ExtRecord& r = ext_of(*e);
        auto v = pack_pool::view(r.handle);
        if (!r.handle || v.size() < r.len) return std::nullopt;    // F1 guard
        PackTensorView t;
        t.width = r.w; t.height = r.h; t.channels = r.c;
        t.dtype = PackDtype(e->type_id);
        t.elem_size = pack_dtype_elem_size(t.dtype);
        t.bytes = v.first(r.len);
        t.handle = r.handle;               // borrowed — see PackTensorView
        return t;
    }
    // User-typed blob: a Bin entry read TOGETHER with its type_id (0 = plain
    // bin, >= kPackTypeUserBase = the producer's declared type). Same storage
    // duality + F1 guard as get_bin — one span either way.
    std::optional<PackBlobView> get_blob(std::string_view key) const {
        const auto* e = find(key);
        if (!e || PackTag(e->tag) != PackTag::Bin) return std::nullopt;
        PackBlobView bv;
        bv.type_id = e->type_id;
        if (e->storage == pack_detail::kStorageExtern) {
            const pack_detail::ExtRecord& r = ext_of(*e);
            auto v = pack_pool::view(r.handle);
            if (!r.handle || v.size() < r.len) return std::nullopt;   // F1 guard
            bv.bytes = v.first(r.len);
        } else {
            bv.bytes = std::span<const uint8_t>(slab() + e->off, e->len);
        }
        return bv;
    }
    // DirEntry::type_id by key (the by-key sibling of type_id_at): nullopt if
    // the key is absent.
    std::optional<uint16_t> type_id_of(std::string_view key) const {
        const auto* e = find(key);
        return e ? std::optional<uint16_t>(e->type_id) : std::nullopt;
    }
    // Element-typed tensor read: nullopt unless the STORED dtype matches T —
    // the producer/consumer contract stays as honest as every typed getter.
    template <class T>
    std::optional<PackTensorOf<T>> get_tensor_of(std::string_view key) const {
        constexpr PackDtype want = pack_dtype_of<T>::value;
        auto v = get_tensor(key);
        if (!v || v->dtype != want) return std::nullopt;
        PackTensorOf<T> t;
        t.data  = reinterpret_cast<const T*>(v->bytes.data());
        t.count = v->bytes.size() / sizeof(T);
        t.width = v->width; t.height = v->height; t.channels = v->channels;
        return t;
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
    // Slab footprint (header + directory + order table + payload).
    size_t slab_bytes()   const { return slab_.data ? size_t(header()->slab_bytes) : 0; }
    // LIVE pool handles this pack owns (failed mints excluded — same count
    // the old handle ledger reported).
    size_t handle_count() const { return slab_.data ? header()->ext_count : 0; }
    // The raw slab (header/dir/order/payload) — the stable block the ABI door
    // may point into. Valid for the Pack's life; STABLE ACROSS Pack MOVES
    // (the slab is one heap allocation the move merely re-owns).
    const uint8_t* slab_data() const { return slab_.data.get(); }

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
    // The i-th entry in INSERTION order (via the ordinal->dir table).
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

    // Binary search on the hash-sorted directory; equal-hash runs are
    // memcmp-verified. Duplicate keys: the run is (hash, key, ordinal)-sorted,
    // so the first key match is the FIRST-INSERTED entry (behavior preserved).
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
        // Drop-on-crash == this exact path: walk the directory, release each
        // pool handle exactly once (single owner), return the slab to the
        // per-thread recycle pool.
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
        o.slab_ = {};    // moved-from owns nothing → never double-releases
    }

    pack_detail::SlabBuf slab_;
};

template <> inline std::optional<int64_t> Pack::get<int64_t>(std::string_view k) const { return get_i64(k); }
template <> inline std::optional<double>  Pack::get<double>(std::string_view k) const  { return get_f64(k); }
template <> inline std::optional<bool>    Pack::get<bool>(std::string_view k) const    { return get_bool(k); }

// ===================================================================
// PackBuilder — the pre-seal, insertion-ordered entry table.
//
// The ONLY way to populate a pack. add_* assert the builder is not yet sealed
// (post-seal writes assert — doc 07 lifecycle step 2). Staging lives in the
// per-thread scratch recycle; seal() sorts the directory, writes the slab in
// one pass, and hands it to an immutable Pack. A builder abandoned without
// seal() (a producer that faults mid-build) releases every pool handle it
// minted or adopted — no leak on the error path.
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

    void add_i64(std::string_view key, int64_t v) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::I64);
        e.off = bump_(8, 8);
        e.len = 8;
        std::memcpy(s_->payload.data() + e.off, &v, 8);
        s_->entries.push_back(e);
    }
    // NaN is flattened to the canonical quiet pattern AT ADD (ruling 1 —
    // the same bit test xi::mp::Writer::float_ applies), so the raw stored
    // double, get_f64, AND the canonical walk all agree byte-for-byte with
    // what the old arena encoding stored.
    void add_f64(std::string_view key, double v) {
        if (spent_()) return;
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
            (bits & 0x000fffffffffffffull) != 0) {
            bits = 0x7ff8000000000000ull;
            std::memcpy(&v, &bits, sizeof v);
        }
        pack_detail::TmpEntry e = begin_(key, PackTag::F64);
        e.off = bump_(8, 8);
        e.len = 8;
        std::memcpy(s_->payload.data() + e.off, &v, 8);
        s_->entries.push_back(e);
    }
    void add_bool(std::string_view key, bool v) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::Bool);
        e.off = bump_(1, 1);
        e.len = 1;
        s_->payload[e.off] = v ? 1 : 0;
        s_->entries.push_back(e);
    }
    void add_str(std::string_view key, std::string_view v) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::Str);
        e.off = bump_(uint32_t(v.size()), 1);
        e.len = uint32_t(v.size());
        if (!v.empty()) std::memcpy(s_->payload.data() + e.off, v.data(), v.size());
        s_->entries.push_back(e);
    }
    // Binary: small stays inline (raw bytes in the slab); large is minted into
    // a pool buffer (D1). Either way get_bin returns one span.
    void add_bin(std::string_view key, const void* data, size_t n) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return;
        if (n >= kPackLargeThreshold) {
            xi_image_handle h = pack_pool::alloc_bytes(data, n);
            if (h) {
                push_extern_(key, PackTag::Bin, 0, h,
                             int32_t(n), 1, 1, uint32_t(n));
                return;
            }
            // Pool exhausted / alloc failed. NEVER store a live-looking extern
            // entry with a null handle — that is the F1 bug where get_bin
            // surfaced a {nullptr, n} span. Fall back to INLINE slab storage
            // so the bytes still ride, honestly (no silent data loss).
            std::fprintf(stderr,
                "[xinsp2] pack add_bin('%.*s'): pool alloc failed for %zu bytes; "
                "storing inline\n",
                int(key.size()), key.data(), n);
        }
        pack_detail::TmpEntry e = begin_(key, PackTag::Bin);
        e.off = bump_(uint32_t(n), 1);
        e.len = uint32_t(n);
        if (n) std::memcpy(s_->payload.data() + e.off, data, n);
        s_->entries.push_back(e);
    }
    // Image: pixels always pooled (aligned raw buffer); descriptor dims ride
    // the entry's ExtRecord. Copies the caller's pixels into a fresh
    // pack-owned buffer. A failed mint stores a null-handle entry (get_image
    // reports empty pixels with the dims kept) — same behavior as before.
    void add_image(std::string_view key, int32_t w, int32_t h, int32_t c,
                   const void* pixels) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return;
        xi_image_handle handle = pack_pool::alloc_image(w, h, c, pixels);
        push_extern_(key, PackTag::Image, 0, handle, w, h, c,
                     uint32_t(int64_t(w) * h * c));
    }
    // Adopt an ALREADY-pooled handle (zero-copy) as an image entry — addref so
    // the pack becomes a co-owner and releases its ref on drop.
    void adopt_image(std::string_view key, int32_t w, int32_t h, int32_t c,
                     xi_image_handle handle) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return;
        pack_pool::addref(handle);
        push_extern_(key, PackTag::Image, 0, handle, w, h, c,
                     uint32_t(int64_t(w) * h * c));
    }
    // Tensor: a first-class typed element buffer — logical shape {w,h,c},
    // dtype in the DirEntry type_id, bytes = w*h*c*elem_size(dtype) copied
    // into a pooled (typeless) buffer. Returns false on mint failure (pool
    // exhausted / zero-sized) — nothing is added (unlike add_bin there is no
    // honest inline fallback for a tensor's typed contract).
    bool add_tensor(std::string_view key, int32_t w, int32_t h, int32_t c,
                    PackDtype dtype, const void* elems) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return false;
        if (w <= 0 || h <= 0 || c <= 0) return false;
        const uint64_t bytes =
            uint64_t(w) * uint64_t(h) * uint64_t(c) * pack_dtype_elem_size(dtype);
        xi_image_handle hnd = pack_pool::alloc_bytes(elems, size_t(bytes));
        if (!hnd) return false;
        push_extern_(key, PackTag::Tensor, uint16_t(dtype), hnd,
                     w, h, c, uint32_t(bytes));
        return true;
    }
    // Adopt an ALREADY-pooled handle (zero-copy) as a TENSOR entry — addref so
    // the pack becomes a co-owner. This is the zero-copy chain the v3 door's
    // get_tensor enables (its view carries the backing handle): consume a
    // tensor from one pack, adopt it into the next, no element copy. Fails
    // closed (false, nothing added, no addref) on a bad shape, a null/dead
    // handle, or a pool buffer smaller than the logical w*h*c*elem_size bytes
    // the shape claims — an adopted tensor can never surface a short span.
    bool adopt_tensor(std::string_view key, int32_t w, int32_t h, int32_t c,
                      PackDtype dtype, xi_image_handle handle) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return false;
        if (w <= 0 || h <= 0 || c <= 0 || !handle) return false;
        const uint64_t bytes =
            uint64_t(w) * uint64_t(h) * uint64_t(c) * pack_dtype_elem_size(dtype);
        if (bytes > uint64_t(INT32_MAX)) return false;
        auto v = pack_pool::view(handle);          // {} for a dead handle
        if (v.size() < size_t(bytes)) return false;
        pack_pool::addref(handle);
        push_extern_(key, PackTag::Tensor, uint16_t(dtype), handle,
                     w, h, c, uint32_t(bytes));
        return true;
    }
    // Adopt an ALREADY-pooled handle (zero-copy) as a Bin entry — the honest
    // sibling of adopt_image for byte payloads (historically a producer had to
    // masquerade a byte buffer as an Image entry with fake dims to get
    // zero-copy adoption). type_id is 0 (plain bin) or the caller's user blob
    // type (>= kPackTypeUserBase) — the zero-copy sibling of add_blob. The
    // entry's logical length is the pool buffer's full byte size. Fails closed
    // on a null/dead handle or a reserved (non-zero, sub-user-base) type_id.
    bool adopt_bin(std::string_view key, uint16_t type_id,
                   xi_image_handle handle) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return false;
        if (!handle) return false;
        if (type_id != 0 && type_id < kPackTypeUserBase) return false;
        auto v = pack_pool::view(handle);          // {} for a dead handle
        if (v.empty() || v.size() > size_t(INT32_MAX)) return false;
        pack_pool::addref(handle);
        push_extern_(key, PackTag::Bin, type_id, handle,
                     int32_t(v.size()), 1, 1, uint32_t(v.size()));
        return true;
    }
    // Typed blob hook (future custom payloads): a Bin-tagged pooled entry
    // carrying a caller-declared type_id (>= kPackTypeUserBase) readable via
    // type_id_at / EntryView::type_id. get_bin resolves it like any large bin.
    bool add_blob(std::string_view key, uint16_t type_id,
                  const void* data, size_t n) {
        assert(!sealed_ && "add after seal");
        if (spent_()) return false;
        assert(type_id >= kPackTypeUserBase && "user blob type_id below the user base");
        xi_image_handle h = pack_pool::alloc_bytes(data, n);
        if (!h) return false;
        push_extern_(key, PackTag::Bin, type_id, h, int32_t(n), 1, 1, uint32_t(n));
        return true;
    }
    // Opaque nested msgpack (already canonical): copied verbatim into the slab.
    //
    // GUARD: this is the path for INTERNAL producers whose bytes are canonical
    // BY CONSTRUCTION (they came out of xi::mp::Writer / a trusted plugin). It
    // does NOT validate — it trusts. FOREIGN / untrusted bytes (inbound comms
    // payloads, third-party chunk data, old replay files) must NOT come here:
    // they go through xi::ingress::canonicalize_entry / canonicalize_into
    // (xi_ingress.hpp), which validates structure, normalizes to the profile,
    // and refuses forged pool-handle ext BEFORE producing the canonical bytes
    // this method then stores. "The safe path is the only path" (doc 07 Ingress).
    void add_mp(std::string_view key, const void* mp, size_t n) {
        if (spent_()) return;
        pack_detail::TmpEntry e = begin_(key, PackTag::Mp);
        e.off = bump_(uint32_t(n), 8);
        e.len = uint32_t(n);
        if (n) std::memcpy(s_->payload.data() + e.off, mp, n);
        s_->entries.push_back(e);
    }

    // Flip immutable: sort the directory, write the slab in one pass, hand it
    // to a Pack. The builder is spent afterwards (scratch recycled).
    Pack seal(int64_t ts_us = 0) {
        assert(!sealed_ && "double seal");
        // A spent/moved-from builder owns no scratch: return an empty Pack
        // deterministically instead of dereferencing a null s_ in a release
        // build (where the assert is compiled out). The invariant is now
        // structural, not assert-only.
        if (spent_()) return Pack{};
        sealed_ = true;
        auto& sc = *s_;
        const uint32_t n = uint32_t(sc.entries.size());
        const uint32_t dir_off     = uint32_t(sizeof(pack_detail::PackHeader));
        const uint32_t order_off   = dir_off + n * uint32_t(sizeof(pack_detail::DirEntry));
        const uint32_t payload_off = (order_off + n * 4u + 7u) & ~7u;   // 8-aligned
        const uint64_t slab_bytes  = payload_off + sc.payload.size();

        pack_detail::SlabBuf slab =
            pack_detail::slab_acquire(size_t(slab_bytes), kDefaultSlab);
        uint8_t* base = slab.data.get();

        // Directory order: (key_hash, key bytes, ordinal) — deterministic,
        // binary-searchable, collision runs adjacent, first-inserted-wins on
        // duplicate keys. The sort permutes an index array; TmpEntry order in
        // the scratch stays insertion order (their index IS the ordinal).
        sort_idx_.resize(n);
        for (uint32_t i = 0; i < n; ++i) sort_idx_[i] = i;
        const uint8_t* pay = sc.payload.data();
        const pack_detail::TmpEntry* te = sc.entries.data();
        std::sort(sort_idx_.begin(), sort_idx_.end(),
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
            const uint32_t ord = sort_idx_[i];
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
            d.type_id  = t.type_id;
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

    // True once the builder is spent (sealed) or moved-from — in both states
    // s_ is null, so every mutator refuses deterministically (see spent-guard
    // at each add_*) instead of dereferencing null in a release build.
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
    // Register an EXTERN entry: its ExtRecord goes into the payload now (the
    // slab copy at seal carries it verbatim); the handle also rides the
    // TmpEntry so an abandoned builder can release it.
    void push_extern_(std::string_view key, PackTag tag, uint16_t type_id,
                      xi_image_handle handle, int32_t w, int32_t h, int32_t c,
                      uint32_t len) {
        pack_detail::TmpEntry e = begin_(key, tag);
        e.storage = pack_detail::kStorageExtern;
        e.type_id = type_id;
        e.handle  = handle;
        e.off     = bump_(uint32_t(sizeof(pack_detail::ExtRecord)), 8);
        e.len     = uint32_t(sizeof(pack_detail::ExtRecord));
        pack_detail::ExtRecord r{handle, w, h, c, len};
        std::memcpy(s_->payload.data() + e.off, &r, sizeof r);
        if (handle) ++ext_live_;
        s_->entries.push_back(e);
    }
    // Bump-allocate n payload bytes at `align`; returns the payload-relative
    // offset. payload_offset is 8-aligned in the slab, so payload-relative
    // alignment survives the rebase for align <= 8.
    uint32_t bump_(uint32_t n, uint32_t align) {
        size_t off = (s_->payload.size() + (align - 1)) & ~size_t(align - 1);
        s_->payload.resize(off + n);
        return uint32_t(off);
    }
    void abandon_() {
        if (!s_) return;
        // A builder abandoned without seal() still owns any handles it minted
        // (e.g. a producer that faults mid-build) — release them, no leak.
        for (const auto& e : s_->entries)
            if (e.storage == pack_detail::kStorageExtern)
                pack_pool::release(e.handle);
        pack_detail::scratch_put(s_);
        s_ = nullptr;
        ext_live_ = 0;
    }

    pack_detail::BuilderScratch* s_ = nullptr;
    std::vector<uint32_t> sort_idx_;   // seal-time permutation (reused capacity)
    uint32_t ext_live_ = 0;            // live (non-null) handles this builder owns
    bool sealed_ = false;
};

} // namespace xi
