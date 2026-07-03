#pragma once
//
// xi_pack.hpp — the v3 uniform keyed-buffer pack container (polaris2 wave-1).
//
// A pack is ONE thing:  key(string) -> entry, where an entry is
// (type tag, const-span bytes). There is no image/metadata split — an image
// is simply an entry whose tag says "image" and whose pixels live in a pool
// buffer. See docs/new_gen/07-uniform-keyed-buffer-plane.md for the decision.
//
// This header implements the CONTAINER SEMANTICS only:
//   * Arena     — per-pack bump allocator; owns every small entry's bytes;
//                 one-shot free at pack end. Its chunks RECYCLE through a
//                 per-thread freelist (ArenaPool), so a steady stream of packs
//                 on one lane's thread stops heap-allocating a fresh chunk per
//                 pack — the ImagePool discipline in miniature (see below).
//   * Builder   — pre-seal, insertion-ordered entry table; the only way to
//                 add entries. A pack under construction is never shareable.
//   * seal()    — flips the pack immutable and (for the dynamic, string-keyed
//                 path) builds the key index; only a sealed Pack crosses to
//                 consumers.
//   * Pack     — an immutable, single-owner value: borrowed const views out,
//                 arena freed + pool handles released on destruction. Drop-on-
//                 crash is EXACTLY destruction (no reconciliation, no COW).
//
// TWO ACCESS PATHS, ONE CONTAINER (doc 07 §profile-1, the wave-1 exit-gate
// condition — docs/new_gen/08 "Wave-1 exit gate — VERDICT"):
//
//   * TypedPack<Schema> — the OFFSET-ACCESSOR read path. When the field set is
//     declared up front (the contract's _keys.h key order), the schema resolves
//     key->slot at COMPILE TIME; each slot holds a direct pointer to its
//     canonical bytes, filled once at set. get_i64<kSeq>() is [slot]->ptr->load
//     — no hash, no scan, no string compare (C3). Declared keys are static
//     constants, so nothing is interned (the per-key intern cost vanishes).
//     This is the path 07's perf claims are about; it is what a generated
//     accessor (wave 3) compiles to.
//
//   * Pack / PackBuilder — the DYNAMIC, string-keyed path, for undeclared or
//     runtime keys (generic walkers, ad-hoc producers, ingress-canonicalized
//     foreign maps). Its index is HYBRID by measurement: a small pack (the hot
//     path) is a linear memcmp scan over the contiguous entry table — which
//     beats a hash map for a handful of keys and allocates NO index nodes — and
//     only a large pack builds an unordered_map to keep lookups O(1) at scale.
//
// The msgpack codec is a SIBLING branch. This file speaks to a deliberately
// tiny internal reader/writer (`pack_mp_detail`) that emits/reads only the
// canonical max-width scalar/str/bin forms the tests need. At integration it
// is replaced wholesale by xi_mp.hpp — the Pack API above it does not change.
//
// Large payloads (images, big binaries) do NOT live in the arena: they are
// raw pool buffers referenced by a handle. The handle mechanics are the
// EXISTING lock-free refcounted ImagePool (xi_image_pool.hpp), reached here
// through a thin TYPELESS facade (`pack_pool`, task 1e). Handles are minted
// ONLY by this pack layer — the privileged ext path of doc 07's ingress rule.
//

#include "xi_abi.h"
#include "xi_image_pool.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace xi {

// ===================================================================
// pack_mp_detail — minimal canonical-profile msgpack reader/writer.
//
//   SWAP TARGET: replace this whole namespace with xi_mp.hpp at codec
//   integration. Fixed-width forms only (int64 0xd3, float64 0xcb,
//   str32 0xdb, bin32 0xc6) — the canonical max-width profile of doc 07,
//   just enough to store scalars/strings/small binaries and read them back.
//   Everything is 100% standard msgpack; a stock decoder reads it.
// ===================================================================
namespace pack_mp_detail {

inline void put_u64_be(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) { p[i] = uint8_t(v & 0xFF); v >>= 8; }
}
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
inline constexpr size_t kI64Size = 9;   // 0xd3 + 8
inline constexpr size_t kF64Size = 9;   // 0xcb + 8
inline size_t str_size(size_t n) { return 5 + n; }  // 0xdb + 4 len + bytes
inline size_t bin_size(size_t n) { return 5 + n; }  // 0xc6 + 4 len + bytes

inline void write_i64(uint8_t* out, int64_t v) {
    out[0] = 0xd3;
    put_u64_be(out + 1, uint64_t(v));
}
inline void write_f64(uint8_t* out, double v) {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    out[0] = 0xcb;
    put_u64_be(out + 1, bits);
}
inline void write_str(uint8_t* out, std::string_view s) {
    out[0] = 0xdb;
    put_u32_be(out + 1, uint32_t(s.size()));
    if (!s.empty()) std::memcpy(out + 5, s.data(), s.size());
}
inline void write_bin(uint8_t* out, const void* data, size_t n) {
    out[0] = 0xc6;
    put_u32_be(out + 1, uint32_t(n));
    if (n) std::memcpy(out + 5, data, n);
}

inline int64_t read_i64(const uint8_t* p) {
    assert(p[0] == 0xd3 && "pack entry is not a canonical int64");
    return int64_t(get_u64_be(p + 1));
}
inline double read_f64(const uint8_t* p) {
    assert(p[0] == 0xcb && "pack entry is not a canonical float64");
    uint64_t bits = get_u64_be(p + 1);
    double v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}
// str/bin readers return a view straight into the arena payload region.
inline std::string_view read_str(const uint8_t* p) {
    assert(p[0] == 0xdb && "pack entry is not a canonical str32");
    uint32_t n = get_u32_be(p + 1);
    return std::string_view(reinterpret_cast<const char*>(p + 5), n);
}
inline std::span<const uint8_t> read_bin(const uint8_t* p) {
    assert(p[0] == 0xc6 && "pack entry is not a canonical bin32");
    uint32_t n = get_u32_be(p + 1);
    return std::span<const uint8_t>(p + 5, n);
}

} // namespace pack_mp_detail

namespace pack_detail {

// ===================================================================
// ArenaPool — a per-thread freelist of recyclable arena buffers.
//
// A pack's arena chunks are BORROWED here and RETURNED here when the pack is
// destroyed, so a steady stream of packs built and dropped on one thread reuses
// the same handful of buffers instead of hitting the heap allocator per pack
// (the third cost the wave-1 verdict named: "a fresh heap arena chunk per
// pack"). It is scoped THREAD-LOCAL — the "per-lane arena cache" the task
// blesses: a pack is built, sealed, read, and dropped within one lane worker's
// thread (see doc 07's dispatch), so the recycle needs no lock. A pack that
// legitimately outlives its producer thread still frees correctly — its buffers
// simply return to whichever thread's pool drops it, or, if that pool is full,
// free outright. The retained set is bounded (kMaxFree) so an idle thread does
// not hoard memory.
// ===================================================================
struct ArenaBuf {
    std::unique_ptr<uint8_t[]> data;
    size_t cap = 0;
};

class ArenaPool {
public:
    // Hand out a buffer with capacity >= need. Prefer a pooled one (LIFO — the
    // hottest in cache); otherwise allocate one of at least the default chunk.
    ArenaBuf acquire(size_t need, size_t default_cap) {
        for (size_t i = free_.size(); i-- > 0;) {
            if (free_[i].cap >= need) {
                ArenaBuf b = std::move(free_[i]);
                free_[i] = std::move(free_.back());
                free_.pop_back();
                return b;
            }
        }
        size_t cap = need > default_cap ? need : default_cap;
        return ArenaBuf{std::unique_ptr<uint8_t[]>(new uint8_t[cap]), cap};
    }
    // Take a buffer back for reuse; drop it (free) if the pool is already full.
    void release(ArenaBuf&& b) {
        if (b.data && free_.size() < kMaxFree) free_.push_back(std::move(b));
    }

private:
    static constexpr size_t kMaxFree = 32;
    std::vector<ArenaBuf> free_;
};

inline ArenaPool& arena_pool() {
    thread_local ArenaPool p;
    return p;
}

} // namespace pack_detail

// ===================================================================
// Arena — per-pack bump allocator (pool-backed chunks, one-shot free).
//
// Owns every small entry's bytes AND the interned key strings (dynamic path).
// Allocation is a pointer bump within the current chunk; growth borrows another
// chunk from the per-thread ArenaPool. The COMMON case — a pack that fits in
// one chunk — keeps its chunk INLINE (head_), so a small pack touches the heap
// allocator zero times (no chunk alloc, no chunk-vector alloc): its buffer came
// from the recycle pool. Destruction returns every chunk to the pool in one
// shot (the pack's "arena dies with the pack" discipline). Move-only: moving a
// pack moves the chunk(s), and because each chunk is a stable heap block, every
// span/string_view handed out into the arena stays valid across the move.
// ===================================================================
class Arena {
public:
    Arena() = default;
    Arena(Arena&& o) noexcept { move_from(std::move(o)); }
    Arena& operator=(Arena&& o) noexcept {
        if (this != &o) { recycle(); move_from(std::move(o)); }
        return *this;
    }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena() { recycle(); }

    uint8_t* alloc(size_t n, size_t align = 8) {
        if (n == 0) n = 1;  // keep every entry's ptr distinct + dereferenceable
        if (extra_.empty()) {
            if (head_.data) {
                size_t off = align_up(head_used_, align);
                if (off + n <= head_.cap) {
                    uint8_t* p = head_.data.get() + off;
                    head_used_ = off + n;
                    used_total_ += n;
                    return p;
                }
            } else {
                head_ = pack_detail::arena_pool().acquire(n, kChunk);
                head_used_ = n;
                used_total_ += n;
                return head_.data.get();
            }
        } else {
            Chunk& c = extra_.back();
            size_t off = align_up(c.used, align);
            if (off + n <= c.buf.cap) {
                uint8_t* p = c.buf.data.get() + off;
                c.used = off + n;
                used_total_ += n;
                return p;
            }
        }
        // Current chunk is full: borrow another from the pool (cap >= n).
        pack_detail::ArenaBuf b = pack_detail::arena_pool().acquire(n, kChunk);
        uint8_t* p = b.data.get();
        extra_.push_back(Chunk{std::move(b), n});
        used_total_ += n;
        return p;
    }

    // Copy s into the arena and return a stable view of the copy.
    std::string_view intern(std::string_view s) {
        uint8_t* p = alloc(s.size(), 1);
        if (!s.empty()) std::memcpy(p, s.data(), s.size());
        return std::string_view(reinterpret_cast<const char*>(p), s.size());
    }

    size_t bytes_used()  const { return used_total_; }
    size_t chunk_count() const { return (head_.data ? 1 : 0) + extra_.size(); }

private:
    struct Chunk {
        pack_detail::ArenaBuf buf;
        size_t used = 0;
    };
    static constexpr size_t kChunk = 4096;
    static size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }

    void recycle() {
        auto& pool = pack_detail::arena_pool();
        if (head_.data) pool.release(std::move(head_));
        for (auto& c : extra_) pool.release(std::move(c.buf));
        head_ = {};
        head_used_ = 0;
        extra_.clear();
        used_total_ = 0;
    }
    void move_from(Arena&& o) noexcept {
        head_      = std::move(o.head_);
        head_used_ = o.head_used_;
        extra_     = std::move(o.extra_);
        used_total_ = o.used_total_;
        o.head_used_  = 0;
        o.used_total_ = 0;
    }

    pack_detail::ArenaBuf head_;        // inline first chunk (no vector alloc)
    size_t head_used_ = 0;
    std::vector<Chunk> extra_;           // spill chunks (only if head overflows)
    size_t used_total_ = 0;
};

// ===================================================================
// pack_pool — thin TYPELESS facade over ImagePool (task 1e).
//
// The pool is image-shaped (create(w,h,ch)); a typeless large buffer of N
// bytes is just a (N,1,1) entry whose pixels ARE the bytes, and an image is a
// native (w,h,c) entry. Either way the handle is minted here, released here,
// and resolved to a raw const span. This is the ONLY mint path in the pack
// layer, satisfying doc 07's "handles are mintable only by the domain's own
// allocator". See docs/new_gen/09-bufferpool-audit.md for the reuse verdict.
// ===================================================================
namespace pack_pool {

// Mint a typeless buffer of n bytes and copy src into it. 0 on failure
// (n==0, over the pool's 1 GiB per-buffer cap, or pool exhausted).
inline xi_image_handle alloc_bytes(const void* src, size_t n) {
    if (n == 0 || n > size_t(INT32_MAX)) return XI_IMAGE_NULL;
    xi_image_handle h = ImagePool::instance().create(int32_t(n), 1, 1);
    if (h && src) std::memcpy(ImagePool::instance().data(h), src, n);
    return h;
}
// Mint an image buffer (w*h*c bytes) and copy pixels in. 0 on failure.
inline xi_image_handle alloc_image(int32_t w, int32_t h, int32_t c, const void* px) {
    xi_image_handle handle = ImagePool::instance().create(w, h, c);
    if (handle && px) {
        std::memcpy(ImagePool::instance().data(handle),
                    px, size_t(w) * size_t(h) * size_t(c));
    }
    return handle;
}
inline void addref(xi_image_handle h) {
    if (h && g_image_pool_alive.load(std::memory_order_acquire))
        ImagePool::instance().addref(h);
}
inline void release(xi_image_handle h) {
    // Guarded so a pack destroyed during static teardown (after the pool
    // singleton is gone) never touches a destroyed Meyers singleton.
    if (h && g_image_pool_alive.load(std::memory_order_acquire))
        ImagePool::instance().release(h);
}
inline std::span<const uint8_t> view(xi_image_handle h) {
    if (!h || !g_image_pool_alive.load(std::memory_order_acquire)) return {};
    auto& pool = ImagePool::instance();
    const uint8_t* p = pool.read_data(h);
    if (!p) return {};
    size_t n = size_t(pool.width(h)) * size_t(pool.height(h)) * size_t(pool.channels(h));
    return std::span<const uint8_t>(p, n);
}

} // namespace pack_pool

// ===================================================================
// Entry type tags. The tag is the entry's stored type; the bytes are its
// canonical-profile encoding (scalars/str/bin inline in the arena) or a
// pool-buffer reference (Bin above threshold, Image). Unknown/opaque nested
// msgpack rides as Mp — forward compatibility by construction (doc 07 §2).
// ===================================================================
enum class PackTag : uint8_t { I64, F64, Str, Bin, Image, Mp };

// A borrowed const view of an image entry: dimensions + a zero-copy span over
// the pool buffer's pixels. Valid for the lifetime of the owning Pack.
struct PackImageView {
    int32_t width    = 0;
    int32_t height   = 0;
    int32_t channels = 0;
    std::span<const uint8_t> pixels;
};

// Bytes at/above this size go to a pool buffer instead of the arena (D1
// storage duality). Small enough that scalars/short strings stay inline;
// large enough that kilobyte metadata does not churn the pool.
inline constexpr size_t kPackLargeThreshold = 4096;

namespace pack_detail {
// One table row for the dynamic, string-keyed path. Insertion-ordered; the key
// is a view into the arena.
struct Entry {
    std::string_view key;
    PackTag tag = PackTag::I64;
    bool     pooled = false;              // storage lives in a pool buffer
    const uint8_t* inl = nullptr;         // arena payload (inline forms)
    uint32_t inl_len = 0;                 // arena payload byte length
    xi_image_handle handle = XI_IMAGE_NULL;  // pool buffer (pooled forms)
    int32_t  w = 0, h = 0, c = 0;         // image descriptor dims
};

// One slot for the TYPED (schema) path. The key is IMPLICIT — it is the slot's
// compile-time position — so no key string is stored and none is interned. A
// slot carries the same payload duality as Entry (inline arena bytes OR a pool
// buffer). `present` distinguishes a set slot from a declared-but-unset one.
struct Slot {
    PackTag tag = PackTag::I64;
    bool     present = false;
    bool     pooled  = false;
    const uint8_t* inl = nullptr;
    uint32_t inl_len = 0;
    xi_image_handle handle = XI_IMAGE_NULL;
    int32_t  w = 0, h = 0, c = 0;
};
} // namespace pack_detail

class PackBuilder;

// ===================================================================
// Pack — a sealed, immutable, single-owner keyed buffer (dynamic path).
//
// Only produced by PackBuilder::seal(); there is no public constructor, so a
// pre-seal (mutable) pack can never be handed out as a Pack. Move-only:
// exactly one owner at a time, whose destruction is the whole lifecycle end —
// arena freed in one shot, every pool handle released once. A moved-from Pack
// owns nothing and releases nothing, so a drop can never double-release.
//
// LOOKUP is hybrid (measured honesty, doc 07 §profile-1 "small maps often beat
// hash with a linear memcmp scan"): a small pack scans the contiguous entry
// table (no index nodes allocated at all); a large pack builds an
// unordered_map so lookups stay O(1) at scale. The declared-field hot path does
// NOT use this container — it uses TypedPack<Schema> below, whose reads are a
// direct slot index with no lookup at all.
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

    // Above this entry count seal() builds an unordered_map; at or below it the
    // pack linear-scans its contiguous table (fewer keys than this, a memcmp
    // scan is faster than a hash lookup AND allocates no index).
    static constexpr size_t kLinearMax = 24;

    // ---- structure ---------------------------------------------------
    size_t size()  const { return entries_.size(); }
    bool   empty() const { return entries_.empty(); }
    bool   has(std::string_view key) const { return find(key) != nullptr; }

    std::optional<PackTag> tag_of(std::string_view key) const {
        const auto* e = find(key);
        return e ? std::optional<PackTag>(e->tag) : std::nullopt;
    }

    // Insertion-ordered key walk — the generic-plugin path (record_save,
    // expose): visit every entry without knowing its producer.
    template <class Fn>
    void for_each(Fn&& fn) const {
        for (const auto& e : entries_) fn(e.key, e.tag);
    }

    // Insertion-ordered index accessors — the O(1) primitives the C-ABI generic
    // walk (xi_pack_v1.key_at/tag_at) is built on, so a foreign consumer can
    // enumerate entries without a producer-supplied key list. UB if i >= size().
    std::string_view key_at(size_t i) const { return entries_[i].key; }
    PackTag         tag_at(size_t i) const { return entries_[i].tag; }

    // Raw stored bytes of the i-th entry's INLINE canonical value — the small-
    // plane bytes exactly as they live in the arena (I64/F64/Str/inline-Bin/Mp).
    // This is the memory plane a generic dumper (expose XEX1-v2, record_save)
    // splices to the wire verbatim, so the wire's small plane EQUALS memory
    // byte-for-byte (doc 07). Empty for a POOLED entry (Image, or a Bin above
    // kPackLargeThreshold) whose payload is a pool buffer — resolve those with
    // get_image / get_bin. UB if i >= size().
    std::span<const uint8_t> raw_at(size_t i) const {
        const auto& e = entries_[i];
        if (e.pooled) return {};
        return std::span<const uint8_t>(e.inl, e.inl_len);
    }

    // ---- typed borrowed reads ----------------------------------------
    std::optional<int64_t> get_i64(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != PackTag::I64) return std::nullopt;
        return pack_mp_detail::read_i64(e->inl);
    }
    std::optional<double> get_f64(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != PackTag::F64) return std::nullopt;
        return pack_mp_detail::read_f64(e->inl);
    }
    std::optional<std::string_view> get_str(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != PackTag::Str) return std::nullopt;
        return pack_mp_detail::read_str(e->inl);
    }
    // Binary: resolves storage duality — inline arena bytes OR a pool buffer,
    // both surfaced as one const span (D1 "storage duality, API unity").
    std::optional<std::span<const uint8_t>> get_bin(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != PackTag::Bin) return std::nullopt;
        if (e->pooled) return pack_pool::view(e->handle).first(e->inl_len);
        return pack_mp_detail::read_bin(e->inl);
    }
    // Image descriptor + zero-copy pixel span over the pool buffer.
    std::optional<PackImageView> get_image(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != PackTag::Image) return std::nullopt;
        return PackImageView{e->w, e->h, e->c, pack_pool::view(e->handle)};
    }
    // Opaque nested msgpack pass-through (unknown type tags, arrays, maps).
    std::optional<std::span<const uint8_t>> get_mp(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != PackTag::Mp) return std::nullopt;
        return std::span<const uint8_t>(e->inl, e->inl_len);
    }

    // Doc-flavored get<i64>/get<f64> aliases (the _keys.h accessor style).
    template <class T> std::optional<T> get(std::string_view key) const;

    // ---- diagnostics -------------------------------------------------
    size_t arena_bytes()   const { return arena_.bytes_used(); }
    size_t handle_count()  const { return handles_.size(); }

private:
    friend class PackBuilder;
    using Entry = pack_detail::Entry;

    Pack(Arena&& arena, std::vector<Entry>&& entries,
          std::vector<xi_image_handle>&& handles)
        : arena_(std::move(arena)), entries_(std::move(entries)),
          handles_(std::move(handles)) {
        // Only a large pack pays for a hash index; small packs scan (below).
        if (entries_.size() > kLinearMax) {
            index_.reserve(entries_.size());
            for (size_t i = 0; i < entries_.size(); ++i)
                index_.emplace(entries_[i].key, i);
        }
    }

    const Entry* find(std::string_view key) const {
        if (index_.empty()) {                 // small pack -> linear scan
            for (const auto& e : entries_)
                if (e.key == key) return &e;
            return nullptr;
        }
        auto it = index_.find(key);           // large pack -> O(1) hash lookup
        return it == index_.end() ? nullptr : &entries_[it->second];
    }

    void destroy() {
        // Drop-on-crash == this exact path: release each pool handle exactly
        // once (single owner), then the arena frees in one shot as members die.
        for (xi_image_handle h : handles_) pack_pool::release(h);
        handles_.clear();
    }
    void move_from(Pack&& o) noexcept {
        arena_   = std::move(o.arena_);
        entries_ = std::move(o.entries_);
        handles_ = std::move(o.handles_);
        index_   = std::move(o.index_);
        o.entries_.clear();
        o.handles_.clear();   // moved-from owns nothing → never double-releases
        o.index_.clear();
    }

    Arena arena_;
    std::vector<Entry> entries_;                 // insertion order
    std::vector<xi_image_handle> handles_;       // the single owner's handles
    std::unordered_map<std::string_view, size_t> index_;  // large packs only
};

template <> inline std::optional<int64_t> Pack::get<int64_t>(std::string_view k) const { return get_i64(k); }
template <> inline std::optional<double>  Pack::get<double>(std::string_view k) const  { return get_f64(k); }

// ===================================================================
// PackBuilder — the pre-seal, insertion-ordered entry table (dynamic path).
//
// The ONLY way to populate a dynamic pack. add_* assert the builder is not yet
// sealed (post-seal writes assert — doc 07 lifecycle step 2). seal() moves the
// arena, table, and handle ledger into an immutable Pack and empties the
// builder, so a builder can neither be sealed twice nor written after seal.
// ===================================================================
class PackBuilder {
public:
    PackBuilder() = default;
    PackBuilder(PackBuilder&&) = default;
    PackBuilder& operator=(PackBuilder&&) = default;
    PackBuilder(const PackBuilder&) = delete;
    PackBuilder& operator=(const PackBuilder&) = delete;
    ~PackBuilder() {
        // A builder abandoned without seal() still owns any handles it minted
        // (e.g. a producer that faults mid-build) — release them, no leak.
        for (xi_image_handle h : handles_) pack_pool::release(h);
    }

    bool sealed() const { return sealed_; }

    void add_i64(std::string_view key, int64_t v) {
        Entry& e = begin_inline(key, PackTag::I64, pack_mp_detail::kI64Size);
        pack_mp_detail::write_i64(const_cast<uint8_t*>(e.inl), v);
    }
    void add_f64(std::string_view key, double v) {
        Entry& e = begin_inline(key, PackTag::F64, pack_mp_detail::kF64Size);
        pack_mp_detail::write_f64(const_cast<uint8_t*>(e.inl), v);
    }
    void add_str(std::string_view key, std::string_view v) {
        Entry& e = begin_inline(key, PackTag::Str, pack_mp_detail::str_size(v.size()));
        pack_mp_detail::write_str(const_cast<uint8_t*>(e.inl), v);
    }
    // Binary: small stays inline (canonical bin32 in the arena); large is
    // minted into a pool buffer (D1). Either way get_bin returns one span.
    void add_bin(std::string_view key, const void* data, size_t n) {
        assert(!sealed_ && "add after seal");
        if (n >= kPackLargeThreshold) {
            xi_image_handle h = pack_pool::alloc_bytes(data, n);
            Entry e;
            e.key = arena_.intern(key);
            e.tag = PackTag::Bin;
            e.pooled = true;
            e.handle = h;
            e.inl_len = uint32_t(n);   // logical byte length within the buffer
            e.w = int32_t(n); e.h = 1; e.c = 1;
            push(std::move(e), h);
            return;
        }
        Entry& e = begin_inline(key, PackTag::Bin, pack_mp_detail::bin_size(n));
        pack_mp_detail::write_bin(const_cast<uint8_t*>(e.inl), data, n);
    }
    // Image: pixels always pooled (aligned raw buffer); descriptor is the
    // dims carried on the entry. Copies the caller's pixels into a fresh
    // pack-owned buffer.
    void add_image(std::string_view key, int32_t w, int32_t h, int32_t c,
                   const void* pixels) {
        assert(!sealed_ && "add after seal");
        xi_image_handle handle = pack_pool::alloc_image(w, h, c, pixels);
        Entry e;
        e.key = arena_.intern(key);
        e.tag = PackTag::Image;
        e.pooled = true;
        e.handle = handle;
        e.w = w; e.h = h; e.c = c;
        push(std::move(e), handle);
    }
    // Adopt an ALREADY-pooled handle (zero-copy) as an image entry — addref so
    // the pack becomes a co-owner and releases its ref on drop.
    void adopt_image(std::string_view key, int32_t w, int32_t h, int32_t c,
                     xi_image_handle handle) {
        assert(!sealed_ && "add after seal");
        pack_pool::addref(handle);
        Entry e;
        e.key = arena_.intern(key);
        e.tag = PackTag::Image;
        e.pooled = true;
        e.handle = handle;
        e.w = w; e.h = h; e.c = c;
        push(std::move(e), handle);
    }
    // Opaque nested msgpack (already canonical): copied verbatim into the arena.
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
        Entry& e = begin_inline(key, PackTag::Mp, n);
        if (n) std::memcpy(const_cast<uint8_t*>(e.inl), mp, n);
    }

    // Flip immutable: hand arena/table/handles to a Pack, empty the builder.
    Pack seal() {
        assert(!sealed_ && "double seal");
        sealed_ = true;
        return Pack(std::move(arena_), std::move(entries_), std::move(handles_));
    }

private:
    using Entry = pack_detail::Entry;

    // Reserve `n` arena bytes for an inline entry, register it, return the row
    // (its `inl` points at the reserved bytes for the caller to fill).
    Entry& begin_inline(std::string_view key, PackTag tag, size_t n) {
        assert(!sealed_ && "add after seal");
        Entry e;
        e.key = arena_.intern(key);
        e.tag = tag;
        e.inl = arena_.alloc(n);
        e.inl_len = uint32_t(n);
        entries_.push_back(std::move(e));
        return entries_.back();
    }
    void push(Entry&& e, xi_image_handle owned) {
        if (owned) handles_.push_back(owned);
        entries_.push_back(std::move(e));
    }

    Arena arena_;
    std::vector<Entry> entries_;
    std::vector<xi_image_handle> handles_;
    bool sealed_ = false;
};

// ===================================================================
// PackSchema<Derived> — a compile-time declared keyset (the CONTRACT key order,
// the _keys.h pattern). A schema is the door to the OFFSET-ACCESSOR read path:
// its keys are known at compile time, so a key resolves to a SLOT (a fixed
// index) at compile time, and TypedPack reads that slot directly — no hash,
// no scan, no string compare, and nothing interned (the keys are the schema's
// own static constants).
//
// A schema is a tiny CRTP struct the plugin (or wave-3 codegen) declares once:
//
//   struct BlobSchema : xi::PackSchema<BlobSchema> {
//       static constexpr std::array<std::string_view, 4> keys = {
//           "threshold", "blob_count", "mean_area", "label" };
//       enum : int { kThreshold, kBlobCount, kMeanArea, kLabel };
//   };
//
// The enum names ARE the compile-time slots; `slot_of("threshold")` gives the
// same index for a string literal (a constant expression). Field ORDER is the
// contract's duty (doc 07 §profile-1 / xi_mp.hpp header) — the schema fixes it.
// ===================================================================
template <class Derived>
struct PackSchema {
    static constexpr size_t slot_count() { return Derived::keys.size(); }
    static constexpr std::string_view name_of(size_t slot) { return Derived::keys[slot]; }

    // Compile-time key -> slot; -1 if the key is not declared. consteval so a
    // call on a literal key is a constant expression usable as a slot index:
    //   f.get_i64<BlobSchema::slot_of("threshold")>()
    static consteval int slot_of(std::string_view key) {
        for (size_t i = 0; i < Derived::keys.size(); ++i)
            if (Derived::keys[i] == key) return int(i);
        return -1;
    }
    // Runtime key -> slot (the string-keyed fallback into declared fields);
    // -1 if not declared. Linear memcmp scan over the (small) key table.
    static int slot_of_runtime(std::string_view key) {
        for (size_t i = 0; i < Derived::keys.size(); ++i)
            if (Derived::keys[i] == key) return int(i);
        return -1;
    }
};

template <class Schema> class TypedPackBuilder;

// ===================================================================
// TypedPack<Schema> — a sealed, immutable pack whose declared fields are read
// by DIRECT SLOT INDEX (the offset-accessor read path). get_i64<kSeq>() is
// slots_[kSeq] -> ptr -> canonical decode: O(1) with no lookup structure at all,
// the C3 claim (doc 07 §profile-1) realized in the container.
//
// Undeclared / dynamic keys are supported too (mixed packs): they live in a
// small string-keyed side list scanned linearly — the general fallback. Same
// single-owner, move-only, one-shot-free lifecycle as Pack; the arena and every
// pool handle (declared slot OR dynamic entry) release exactly once on drop.
// ===================================================================
template <class Schema>
class TypedPack {
public:
    static constexpr size_t N = Schema::slot_count();
    using Slot  = pack_detail::Slot;
    using Entry = pack_detail::Entry;

    TypedPack() = default;
    TypedPack(TypedPack&& o) noexcept { move_from(std::move(o)); }
    TypedPack& operator=(TypedPack&& o) noexcept {
        if (this != &o) { destroy(); move_from(std::move(o)); }
        return *this;
    }
    TypedPack(const TypedPack&) = delete;
    TypedPack& operator=(const TypedPack&) = delete;
    ~TypedPack() { destroy(); }

    // ---- structure ---------------------------------------------------
    // Count of fields actually SET (declared slots present + dynamic entries).
    size_t size() const {
        size_t n = dyn_.size();
        for (const auto& s : slots_) if (s.present) ++n;
        return n;
    }
    bool empty() const { return size() == 0; }

    template <int SlotIdx>
    bool has() const {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        return slots_[SlotIdx].present;
    }
    bool has(std::string_view key) const {
        int s = Schema::slot_of_runtime(key);
        if (s >= 0) return slots_[s].present;
        return find_dyn(key) != nullptr;
    }

    template <int SlotIdx>
    std::optional<PackTag> tag_of() const {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        const Slot& s = slots_[SlotIdx];
        return s.present ? std::optional<PackTag>(s.tag) : std::nullopt;
    }

    // Walk every set field: declared slots in schema order, then dynamic keys in
    // insertion order — the generic producer-agnostic path (expose, record_save).
    template <class Fn>
    void for_each(Fn&& fn) const {
        for (size_t i = 0; i < N; ++i)
            if (slots_[i].present) fn(Schema::name_of(i), slots_[i].tag);
        for (const auto& e : dyn_) fn(e.key, e.tag);
    }

    // ---- typed slot reads: [slot] -> ptr -> canonical decode (the fast path) --
    template <int SlotIdx>
    std::optional<int64_t> get_i64() const {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        const Slot& s = slots_[SlotIdx];
        if (!s.present || s.tag != PackTag::I64) return std::nullopt;
        return pack_mp_detail::read_i64(s.inl);
    }
    template <int SlotIdx>
    std::optional<double> get_f64() const {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        const Slot& s = slots_[SlotIdx];
        if (!s.present || s.tag != PackTag::F64) return std::nullopt;
        return pack_mp_detail::read_f64(s.inl);
    }
    template <int SlotIdx>
    std::optional<std::string_view> get_str() const {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        const Slot& s = slots_[SlotIdx];
        if (!s.present || s.tag != PackTag::Str) return std::nullopt;
        return pack_mp_detail::read_str(s.inl);
    }
    template <int SlotIdx>
    std::optional<std::span<const uint8_t>> get_bin() const {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        const Slot& s = slots_[SlotIdx];
        if (!s.present || s.tag != PackTag::Bin) return std::nullopt;
        if (s.pooled) return pack_pool::view(s.handle).first(s.inl_len);
        return pack_mp_detail::read_bin(s.inl);
    }
    template <int SlotIdx>
    std::optional<PackImageView> get_image() const {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        const Slot& s = slots_[SlotIdx];
        if (!s.present || s.tag != PackTag::Image) return std::nullopt;
        return PackImageView{s.w, s.h, s.c, pack_pool::view(s.handle)};
    }
    template <int SlotIdx>
    std::optional<std::span<const uint8_t>> get_mp() const {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        const Slot& s = slots_[SlotIdx];
        if (!s.present || s.tag != PackTag::Mp) return std::nullopt;
        return std::span<const uint8_t>(s.inl, s.inl_len);
    }

    // Doc-flavored get<i64, kSeq>() / get<f64, kScore>() aliases.
    template <class T, int SlotIdx> std::optional<T> get() const;

    // ---- string-keyed fallback (undeclared/dynamic keys, or runtime keys) ----
    // A declared key resolves through the slot; an undeclared one scans the
    // dynamic side list. This is the SLOW path — the fast path is get_i64<Slot>().
    std::optional<int64_t> get_i64(std::string_view key) const {
        int s = Schema::slot_of_runtime(key);
        if (s >= 0) return slot_i64(slots_[s]);
        const Entry* e = find_dyn(key);
        if (!e || e->tag != PackTag::I64) return std::nullopt;
        return pack_mp_detail::read_i64(e->inl);
    }
    std::optional<double> get_f64(std::string_view key) const {
        int s = Schema::slot_of_runtime(key);
        if (s >= 0) return slot_f64(slots_[s]);
        const Entry* e = find_dyn(key);
        if (!e || e->tag != PackTag::F64) return std::nullopt;
        return pack_mp_detail::read_f64(e->inl);
    }
    std::optional<std::string_view> get_str(std::string_view key) const {
        int s = Schema::slot_of_runtime(key);
        if (s >= 0) return slot_str(slots_[s]);
        const Entry* e = find_dyn(key);
        if (!e || e->tag != PackTag::Str) return std::nullopt;
        return pack_mp_detail::read_str(e->inl);
    }

    // ---- diagnostics -------------------------------------------------
    size_t arena_bytes()  const { return arena_.bytes_used(); }
    size_t handle_count() const {
        size_t n = 0;
        for (const auto& s : slots_) if (s.present && s.handle) ++n;
        for (const auto& e : dyn_)   if (e.handle) ++n;
        return n;
    }

private:
    friend class TypedPackBuilder<Schema>;

    TypedPack(Arena&& arena, const std::array<Slot, N>& slots,
               std::vector<Entry>&& dyn)
        : arena_(std::move(arena)), slots_(slots), dyn_(std::move(dyn)) {}

    static std::optional<int64_t> slot_i64(const Slot& s) {
        if (!s.present || s.tag != PackTag::I64) return std::nullopt;
        return pack_mp_detail::read_i64(s.inl);
    }
    static std::optional<double> slot_f64(const Slot& s) {
        if (!s.present || s.tag != PackTag::F64) return std::nullopt;
        return pack_mp_detail::read_f64(s.inl);
    }
    static std::optional<std::string_view> slot_str(const Slot& s) {
        if (!s.present || s.tag != PackTag::Str) return std::nullopt;
        return pack_mp_detail::read_str(s.inl);
    }
    const Entry* find_dyn(std::string_view key) const {
        for (const auto& e : dyn_) if (e.key == key) return &e;
        return nullptr;
    }

    void destroy() {
        for (Slot& s : slots_)
            if (s.present && s.handle) { pack_pool::release(s.handle); s.handle = XI_IMAGE_NULL; }
        for (Entry& e : dyn_)
            if (e.handle) pack_pool::release(e.handle);
        dyn_.clear();
    }
    void move_from(TypedPack&& o) noexcept {
        arena_ = std::move(o.arena_);
        slots_ = o.slots_;
        dyn_   = std::move(o.dyn_);
        o.slots_ = {};        // moved-from owns nothing → never double-releases
        o.dyn_.clear();
    }

    Arena arena_;
    std::array<Slot, N> slots_{};   // declared fields, indexed by compile-time slot
    std::vector<Entry> dyn_;        // undeclared/dynamic keys (empty for pure schemas)
};

template <class Schema>
template <class T, int SlotIdx>
std::optional<T> TypedPack<Schema>::get() const {
    if constexpr (std::is_same_v<T, int64_t>) return get_i64<SlotIdx>();
    else if constexpr (std::is_same_v<T, double>) return get_f64<SlotIdx>();
    else { static_assert(sizeof(T) == 0, "TypedPack::get<T,slot> supports int64_t / double"); }
}

// ===================================================================
// TypedPackBuilder<Schema> — populate a schema pack by SLOT. set_i64<kSeq>(v)
// writes canonical bytes into the arena and points the slot at them; the KEY IS
// NEVER STORED OR INTERNED (it is the schema's static constant). Undeclared keys
// route to add_*(key, ...) into the dynamic side list (interned there, the
// general fallback). seal() flips immutable into a TypedPack.
// ===================================================================
template <class Schema>
class TypedPackBuilder {
public:
    static constexpr size_t N = Schema::slot_count();
    using Slot  = pack_detail::Slot;
    using Entry = pack_detail::Entry;

    TypedPackBuilder() = default;
    TypedPackBuilder(TypedPackBuilder&&) = default;
    TypedPackBuilder& operator=(TypedPackBuilder&&) = default;
    TypedPackBuilder(const TypedPackBuilder&) = delete;
    TypedPackBuilder& operator=(const TypedPackBuilder&) = delete;
    ~TypedPackBuilder() {
        // Abandoned without seal(): release handles minted into slots + dyn list.
        for (Slot& s : slots_) if (s.present && s.handle) pack_pool::release(s.handle);
        for (Entry& e : dyn_)  if (e.handle) pack_pool::release(e.handle);
    }

    bool sealed() const { return sealed_; }

    // ---- typed slot setters (no key interned; slot is compile-time) ----------
    template <int SlotIdx>
    void set_i64(int64_t v) {
        Slot& s = begin_slot<SlotIdx>(PackTag::I64, pack_mp_detail::kI64Size);
        pack_mp_detail::write_i64(const_cast<uint8_t*>(s.inl), v);
    }
    template <int SlotIdx>
    void set_f64(double v) {
        Slot& s = begin_slot<SlotIdx>(PackTag::F64, pack_mp_detail::kF64Size);
        pack_mp_detail::write_f64(const_cast<uint8_t*>(s.inl), v);
    }
    template <int SlotIdx>
    void set_str(std::string_view v) {
        Slot& s = begin_slot<SlotIdx>(PackTag::Str, pack_mp_detail::str_size(v.size()));
        pack_mp_detail::write_str(const_cast<uint8_t*>(s.inl), v);
    }
    template <int SlotIdx>
    void set_bin(const void* data, size_t n) {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        assert(!sealed_ && "add after seal");
        Slot& s = slots_[SlotIdx];
        if (n >= kPackLargeThreshold) {
            xi_image_handle h = pack_pool::alloc_bytes(data, n);
            s.tag = PackTag::Bin; s.present = true; s.pooled = true;
            s.handle = h; s.inl_len = uint32_t(n);
            s.w = int32_t(n); s.h = 1; s.c = 1;
            return;
        }
        s = Slot{};
        s.tag = PackTag::Bin; s.present = true;
        s.inl = arena_.alloc(pack_mp_detail::bin_size(n));
        s.inl_len = uint32_t(pack_mp_detail::bin_size(n));
        pack_mp_detail::write_bin(const_cast<uint8_t*>(s.inl), data, n);
    }
    template <int SlotIdx>
    void set_image(int32_t w, int32_t h, int32_t c, const void* pixels) {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        assert(!sealed_ && "add after seal");
        xi_image_handle handle = pack_pool::alloc_image(w, h, c, pixels);
        Slot& s = slots_[SlotIdx];
        s.tag = PackTag::Image; s.present = true; s.pooled = true;
        s.handle = handle; s.w = w; s.h = h; s.c = c; s.inl = nullptr; s.inl_len = 0;
    }
    template <int SlotIdx>
    void adopt_image(int32_t w, int32_t h, int32_t c, xi_image_handle handle) {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        assert(!sealed_ && "add after seal");
        pack_pool::addref(handle);
        Slot& s = slots_[SlotIdx];
        s.tag = PackTag::Image; s.present = true; s.pooled = true;
        s.handle = handle; s.w = w; s.h = h; s.c = c; s.inl = nullptr; s.inl_len = 0;
    }
    // Opaque nested canonical msgpack (trusted-internal; foreign bytes go through
    // xi::ingress first, exactly as add_mp on the dynamic path — see Pack above).
    template <int SlotIdx>
    void set_mp(const void* mp, size_t n) {
        Slot& s = begin_slot<SlotIdx>(PackTag::Mp, n);
        if (n) std::memcpy(const_cast<uint8_t*>(s.inl), mp, n);
    }

    // ---- dynamic (undeclared) keys — the string-keyed fallback ---------------
    void add_i64(std::string_view key, int64_t v) {
        Entry& e = begin_dyn(key, PackTag::I64, pack_mp_detail::kI64Size);
        pack_mp_detail::write_i64(const_cast<uint8_t*>(e.inl), v);
    }
    void add_f64(std::string_view key, double v) {
        Entry& e = begin_dyn(key, PackTag::F64, pack_mp_detail::kF64Size);
        pack_mp_detail::write_f64(const_cast<uint8_t*>(e.inl), v);
    }
    void add_str(std::string_view key, std::string_view v) {
        Entry& e = begin_dyn(key, PackTag::Str, pack_mp_detail::str_size(v.size()));
        pack_mp_detail::write_str(const_cast<uint8_t*>(e.inl), v);
    }
    void add_mp(std::string_view key, const void* mp, size_t n) {
        Entry& e = begin_dyn(key, PackTag::Mp, n);
        if (n) std::memcpy(const_cast<uint8_t*>(e.inl), mp, n);
    }

    TypedPack<Schema> seal() {
        assert(!sealed_ && "double seal");
        sealed_ = true;
        TypedPack<Schema> f(std::move(arena_), slots_, std::move(dyn_));
        slots_ = {};      // ownership moved to the pack; builder releases nothing
        dyn_.clear();
        return f;
    }

private:
    template <int SlotIdx>
    Slot& begin_slot(PackTag tag, size_t n) {
        static_assert(SlotIdx >= 0 && SlotIdx < (int)N, "slot not declared in schema");
        assert(!sealed_ && "add after seal");
        Slot& s = slots_[SlotIdx];
        s = Slot{};
        s.tag = tag;
        s.present = true;
        s.inl = arena_.alloc(n);
        s.inl_len = uint32_t(n);
        return s;
    }
    Entry& begin_dyn(std::string_view key, PackTag tag, size_t n) {
        assert(!sealed_ && "add after seal");
        Entry e;
        e.key = arena_.intern(key);
        e.tag = tag;
        e.inl = arena_.alloc(n);
        e.inl_len = uint32_t(n);
        dyn_.push_back(std::move(e));
        return dyn_.back();
    }

    Arena arena_;
    std::array<Slot, N> slots_{};
    std::vector<Entry> dyn_;
    bool sealed_ = false;
};

} // namespace xi
