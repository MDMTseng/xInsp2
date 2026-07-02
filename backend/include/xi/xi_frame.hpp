#pragma once
//
// xi_frame.hpp — the v3 uniform keyed-buffer frame container (polaris2 wave-1).
//
// A frame is ONE thing:  key(string) -> entry, where an entry is
// (type tag, const-span bytes). There is no image/metadata split — an image
// is simply an entry whose tag says "image" and whose pixels live in a pool
// buffer. See docs/new_gen/07-uniform-keyed-buffer-plane.md for the decision.
//
// This header implements the CONTAINER SEMANTICS only:
//   * Arena     — per-frame bump allocator; owns every small entry's bytes;
//                 one-shot free at frame end.
//   * Builder   — pre-seal, insertion-ordered entry table; the only way to
//                 add entries. A frame under construction is never shareable.
//   * seal()    — flips the frame immutable and builds the O(1) key index;
//                 only a sealed Frame crosses to consumers.
//   * Frame     — an immutable, single-owner value: borrowed const views out,
//                 arena freed + pool handles released on destruction. Drop-on-
//                 crash is EXACTLY destruction (no reconciliation, no COW).
//
// The msgpack codec is a SIBLING branch. This file speaks to a deliberately
// tiny internal reader/writer (`frame_mp_detail`) that emits/reads only the
// canonical max-width scalar/str/bin forms the tests need. At integration it
// is replaced wholesale by xi_mp.hpp — the Frame API above it does not change.
//
// Large payloads (images, big binaries) do NOT live in the arena: they are
// raw pool buffers referenced by a handle. The handle mechanics are the
// EXISTING lock-free refcounted ImagePool (xi_image_pool.hpp), reached here
// through a thin TYPELESS facade (`frame_pool`, task 1e). Handles are minted
// ONLY by this frame layer — the privileged ext path of doc 07's ingress rule.
//

#include "xi_abi.h"
#include "xi_image_pool.hpp"

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
// frame_mp_detail — minimal canonical-profile msgpack reader/writer.
//
//   SWAP TARGET: replace this whole namespace with xi_mp.hpp at codec
//   integration. Fixed-width forms only (int64 0xd3, float64 0xcb,
//   str32 0xdb, bin32 0xc6) — the canonical max-width profile of doc 07,
//   just enough to store scalars/strings/small binaries and read them back.
//   Everything is 100% standard msgpack; a stock decoder reads it.
// ===================================================================
namespace frame_mp_detail {

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
    assert(p[0] == 0xd3 && "frame entry is not a canonical int64");
    return int64_t(get_u64_be(p + 1));
}
inline double read_f64(const uint8_t* p) {
    assert(p[0] == 0xcb && "frame entry is not a canonical float64");
    uint64_t bits = get_u64_be(p + 1);
    double v;
    std::memcpy(&v, &bits, sizeof v);
    return v;
}
// str/bin readers return a view straight into the arena payload region.
inline std::string_view read_str(const uint8_t* p) {
    assert(p[0] == 0xdb && "frame entry is not a canonical str32");
    uint32_t n = get_u32_be(p + 1);
    return std::string_view(reinterpret_cast<const char*>(p + 5), n);
}
inline std::span<const uint8_t> read_bin(const uint8_t* p) {
    assert(p[0] == 0xc6 && "frame entry is not a canonical bin32");
    uint32_t n = get_u32_be(p + 1);
    return std::span<const uint8_t>(p + 5, n);
}

} // namespace frame_mp_detail

// ===================================================================
// Arena — per-frame bump allocator (chunked growth, one-shot free).
//
// Owns every small entry's bytes AND the interned key strings. Allocation is
// a pointer bump within the current chunk; growth adds a chunk. Destruction
// frees all chunks in one shot (the frame's "arena dies with the frame"
// discipline). Move-only: moving a frame moves the chunk vector, and because
// each chunk is a stable heap block, every span/string_view handed out into
// the arena stays valid across the move.
// ===================================================================
class Arena {
public:
    Arena() = default;
    Arena(Arena&&) noexcept = default;
    Arena& operator=(Arena&&) noexcept = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    uint8_t* alloc(size_t n, size_t align = 8) {
        if (n == 0) n = 1;  // keep every entry's ptr distinct + dereferenceable
        Chunk* c = chunks_.empty() ? nullptr : &chunks_.back();
        size_t off = c ? align_up(c->used, align) : 0;
        if (!c || off + n > c->cap) {
            size_t cap = n > kChunk ? n : kChunk;
            chunks_.push_back(Chunk{std::unique_ptr<uint8_t[]>(new uint8_t[cap]), cap, 0});
            c = &chunks_.back();
            off = 0;
        }
        uint8_t* p = c->data.get() + off;
        c->used = off + n;
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
    size_t chunk_count() const { return chunks_.size(); }

private:
    struct Chunk {
        std::unique_ptr<uint8_t[]> data;
        size_t cap  = 0;
        size_t used = 0;
    };
    static constexpr size_t kChunk = 4096;
    static size_t align_up(size_t v, size_t a) { return (v + (a - 1)) & ~(a - 1); }

    std::vector<Chunk> chunks_;
    size_t used_total_ = 0;
};

// ===================================================================
// frame_pool — thin TYPELESS facade over ImagePool (task 1e).
//
// The pool is image-shaped (create(w,h,ch)); a typeless large buffer of N
// bytes is just a (N,1,1) entry whose pixels ARE the bytes, and an image is a
// native (w,h,c) entry. Either way the handle is minted here, released here,
// and resolved to a raw const span. This is the ONLY mint path in the frame
// layer, satisfying doc 07's "handles are mintable only by the domain's own
// allocator". See docs/new_gen/09-bufferpool-audit.md for the reuse verdict.
// ===================================================================
namespace frame_pool {

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
    // Guarded so a frame destroyed during static teardown (after the pool
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

} // namespace frame_pool

// ===================================================================
// Entry type tags. The tag is the entry's stored type; the bytes are its
// canonical-profile encoding (scalars/str/bin inline in the arena) or a
// pool-buffer reference (Bin above threshold, Image). Unknown/opaque nested
// msgpack rides as Mp — forward compatibility by construction (doc 07 §2).
// ===================================================================
enum class FrameTag : uint8_t { I64, F64, Str, Bin, Image, Mp };

// A borrowed const view of an image entry: dimensions + a zero-copy span over
// the pool buffer's pixels. Valid for the lifetime of the owning Frame.
struct FrameImageView {
    int32_t width    = 0;
    int32_t height   = 0;
    int32_t channels = 0;
    std::span<const uint8_t> pixels;
};

// Bytes at/above this size go to a pool buffer instead of the arena (D1
// storage duality). Small enough that scalars/short strings stay inline;
// large enough that kilobyte metadata does not churn the pool.
inline constexpr size_t kFrameLargeThreshold = 4096;

namespace frame_detail {
// One table row. Insertion-ordered; the key is a view into the arena.
struct Entry {
    std::string_view key;
    FrameTag tag = FrameTag::I64;
    bool     pooled = false;              // storage lives in a pool buffer
    const uint8_t* inl = nullptr;         // arena payload (inline forms)
    uint32_t inl_len = 0;                 // arena payload byte length
    xi_image_handle handle = XI_IMAGE_NULL;  // pool buffer (pooled forms)
    int32_t  w = 0, h = 0, c = 0;         // image descriptor dims
};
} // namespace frame_detail

class FrameBuilder;

// ===================================================================
// Frame — a sealed, immutable, single-owner keyed buffer.
//
// Only produced by FrameBuilder::seal(); there is no public constructor, so a
// pre-seal (mutable) frame can never be handed out as a Frame. Move-only:
// exactly one owner at a time, whose destruction is the whole lifecycle end —
// arena freed in one shot, every pool handle released once. A moved-from Frame
// owns nothing and releases nothing, so a drop can never double-release.
// ===================================================================
class Frame {
public:
    Frame() = default;   // empty/null frame (default-constructed, owns nothing)
    Frame(Frame&& o) noexcept { move_from(std::move(o)); }
    Frame& operator=(Frame&& o) noexcept {
        if (this != &o) { destroy(); move_from(std::move(o)); }
        return *this;
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    ~Frame() { destroy(); }

    // ---- structure ---------------------------------------------------
    size_t size()  const { return entries_.size(); }
    bool   empty() const { return entries_.empty(); }
    bool   has(std::string_view key) const { return index_.find(key) != index_.end(); }

    std::optional<FrameTag> tag_of(std::string_view key) const {
        const auto* e = find(key);
        return e ? std::optional<FrameTag>(e->tag) : std::nullopt;
    }

    // Insertion-ordered key walk — the generic-plugin path (record_save,
    // expose): visit every entry without knowing its producer.
    template <class Fn>
    void for_each(Fn&& fn) const {
        for (const auto& e : entries_) fn(e.key, e.tag);
    }

    // ---- typed borrowed reads (O(1) after seal) ----------------------
    std::optional<int64_t> get_i64(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != FrameTag::I64) return std::nullopt;
        return frame_mp_detail::read_i64(e->inl);
    }
    std::optional<double> get_f64(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != FrameTag::F64) return std::nullopt;
        return frame_mp_detail::read_f64(e->inl);
    }
    std::optional<std::string_view> get_str(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != FrameTag::Str) return std::nullopt;
        return frame_mp_detail::read_str(e->inl);
    }
    // Binary: resolves storage duality — inline arena bytes OR a pool buffer,
    // both surfaced as one const span (D1 "storage duality, API unity").
    std::optional<std::span<const uint8_t>> get_bin(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != FrameTag::Bin) return std::nullopt;
        if (e->pooled) return frame_pool::view(e->handle).first(e->inl_len);
        return frame_mp_detail::read_bin(e->inl);
    }
    // Image descriptor + zero-copy pixel span over the pool buffer.
    std::optional<FrameImageView> get_image(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != FrameTag::Image) return std::nullopt;
        return FrameImageView{e->w, e->h, e->c, frame_pool::view(e->handle)};
    }
    // Opaque nested msgpack pass-through (unknown type tags, arrays, maps).
    std::optional<std::span<const uint8_t>> get_mp(std::string_view key) const {
        const auto* e = find(key);
        if (!e || e->tag != FrameTag::Mp) return std::nullopt;
        return std::span<const uint8_t>(e->inl, e->inl_len);
    }

    // Doc-flavored get<i64>/get<f64> aliases (the _keys.h accessor style).
    template <class T> std::optional<T> get(std::string_view key) const;

    // ---- diagnostics -------------------------------------------------
    size_t arena_bytes()   const { return arena_.bytes_used(); }
    size_t handle_count()  const { return handles_.size(); }

private:
    friend class FrameBuilder;
    using Entry = frame_detail::Entry;

    Frame(Arena&& arena, std::vector<Entry>&& entries,
          std::vector<xi_image_handle>&& handles)
        : arena_(std::move(arena)), entries_(std::move(entries)),
          handles_(std::move(handles)) {
        index_.reserve(entries_.size());
        for (size_t i = 0; i < entries_.size(); ++i)
            index_.emplace(entries_[i].key, i);
    }

    const Entry* find(std::string_view key) const {
        auto it = index_.find(key);
        return it == index_.end() ? nullptr : &entries_[it->second];
    }

    void destroy() {
        // Drop-on-crash == this exact path: release each pool handle exactly
        // once (single owner), then the arena frees in one shot as members die.
        for (xi_image_handle h : handles_) frame_pool::release(h);
        handles_.clear();
    }
    void move_from(Frame&& o) noexcept {
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
    std::unordered_map<std::string_view, size_t> index_;  // key -> entry idx (O(1))
};

template <> inline std::optional<int64_t> Frame::get<int64_t>(std::string_view k) const { return get_i64(k); }
template <> inline std::optional<double>  Frame::get<double>(std::string_view k) const  { return get_f64(k); }

// ===================================================================
// FrameBuilder — the pre-seal, insertion-ordered entry table.
//
// The ONLY way to populate a frame. add_* assert the builder is not yet
// sealed (post-seal writes assert — doc 07 lifecycle step 2). seal() moves
// the arena, table, and handle ledger into an immutable Frame and empties the
// builder, so a builder can neither be sealed twice nor written after seal.
// ===================================================================
class FrameBuilder {
public:
    FrameBuilder() = default;
    FrameBuilder(FrameBuilder&&) = default;
    FrameBuilder& operator=(FrameBuilder&&) = default;
    FrameBuilder(const FrameBuilder&) = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;
    ~FrameBuilder() {
        // A builder abandoned without seal() still owns any handles it minted
        // (e.g. a producer that faults mid-build) — release them, no leak.
        for (xi_image_handle h : handles_) frame_pool::release(h);
    }

    bool sealed() const { return sealed_; }

    void add_i64(std::string_view key, int64_t v) {
        Entry& e = begin_inline(key, FrameTag::I64, frame_mp_detail::kI64Size);
        frame_mp_detail::write_i64(const_cast<uint8_t*>(e.inl), v);
    }
    void add_f64(std::string_view key, double v) {
        Entry& e = begin_inline(key, FrameTag::F64, frame_mp_detail::kF64Size);
        frame_mp_detail::write_f64(const_cast<uint8_t*>(e.inl), v);
    }
    void add_str(std::string_view key, std::string_view v) {
        Entry& e = begin_inline(key, FrameTag::Str, frame_mp_detail::str_size(v.size()));
        frame_mp_detail::write_str(const_cast<uint8_t*>(e.inl), v);
    }
    // Binary: small stays inline (canonical bin32 in the arena); large is
    // minted into a pool buffer (D1). Either way get_bin returns one span.
    void add_bin(std::string_view key, const void* data, size_t n) {
        assert(!sealed_ && "add after seal");
        if (n >= kFrameLargeThreshold) {
            xi_image_handle h = frame_pool::alloc_bytes(data, n);
            Entry e;
            e.key = arena_.intern(key);
            e.tag = FrameTag::Bin;
            e.pooled = true;
            e.handle = h;
            e.inl_len = uint32_t(n);   // logical byte length within the buffer
            e.w = int32_t(n); e.h = 1; e.c = 1;
            push(std::move(e), h);
            return;
        }
        Entry& e = begin_inline(key, FrameTag::Bin, frame_mp_detail::bin_size(n));
        frame_mp_detail::write_bin(const_cast<uint8_t*>(e.inl), data, n);
    }
    // Image: pixels always pooled (aligned raw buffer); descriptor is the
    // dims carried on the entry. Copies the caller's pixels into a fresh
    // frame-owned buffer.
    void add_image(std::string_view key, int32_t w, int32_t h, int32_t c,
                   const void* pixels) {
        assert(!sealed_ && "add after seal");
        xi_image_handle handle = frame_pool::alloc_image(w, h, c, pixels);
        Entry e;
        e.key = arena_.intern(key);
        e.tag = FrameTag::Image;
        e.pooled = true;
        e.handle = handle;
        e.w = w; e.h = h; e.c = c;
        push(std::move(e), handle);
    }
    // Adopt an ALREADY-pooled handle (zero-copy) as an image entry — addref so
    // the frame becomes a co-owner and releases its ref on drop.
    void adopt_image(std::string_view key, int32_t w, int32_t h, int32_t c,
                     xi_image_handle handle) {
        assert(!sealed_ && "add after seal");
        frame_pool::addref(handle);
        Entry e;
        e.key = arena_.intern(key);
        e.tag = FrameTag::Image;
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
        Entry& e = begin_inline(key, FrameTag::Mp, n);
        if (n) std::memcpy(const_cast<uint8_t*>(e.inl), mp, n);
    }

    // Flip immutable: hand arena/table/handles to a Frame, empty the builder.
    Frame seal() {
        assert(!sealed_ && "double seal");
        sealed_ = true;
        return Frame(std::move(arena_), std::move(entries_), std::move(handles_));
    }

private:
    using Entry = frame_detail::Entry;

    // Reserve `n` arena bytes for an inline entry, register it, return the row
    // (its `inl` points at the reserved bytes for the caller to fill).
    Entry& begin_inline(std::string_view key, FrameTag tag, size_t n) {
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

} // namespace xi
