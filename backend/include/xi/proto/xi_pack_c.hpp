#pragma once
//
// xi_pack_c.hpp — "Pack design C" PROTOTYPE (proto/pack-c). NOT production.
//
// A candidate next-gen pack representation, built to be BENCHMARKED against
// the current xi_pack.hpp Pack/PackBuilder (see bench_pack_c.cpp), not to be
// integrated. Everything lives in this one self-contained header:
//
//   One pack = ONE contiguous SLAB + N external typed buffers.
//
//   * Slab layout: 64B PackHeaderC -> fixed 32B DirEntryC array (key_hash-
//     sorted, binary search) -> payload region (bump-allocated, per-entry
//     aligned). INLINE entries store scalars RAW (i64/f64 = 8 bytes; read is
//     a pointer add + 8-byte load, zero decode), strings raw, small structured
//     trees as opaque canonical msgpack bytes (encoded by the caller with
//     xi::mp::Writer — this header does not depend on xi_mp.hpp).
//   * EXTERN entries (tensors / large blobs): 64B-aligned buffers from a
//     size-class pool (2^n classes, 4 KiB..64 MiB) with a per-thread magazine
//     (small LIFO cache per class) over a global overflow shelf. Refcounted
//     (atomic rc), carrying {type_id, shape[3], bytes}. Slot+generation
//     handle table — the ImagePool discipline (index|generation packed
//     handle, generation bump on reuse, lock-free resolve).
//   * Pack handles: same slot+generation discipline. retain/release = one
//     atomic on the pack slot; destroying a pack walks the directory,
//     releases EXTERN child refs, and returns the slab to its size class.
//   * serialize = slab verbatim + each EXTERN buffer appended
//     (type_id+shape+len+bytes); deserialize re-mints the EXTERN buffers and
//     patches the directory handles in the fresh slab copy.
//   * RAII refs (PackRef/BufRef) are the C++-side ownership default —
//     copy=retain, move=steal, dtor=release; the raw handle APIs remain as
//     the C-ABI wire shape.
//   * Tensors carry a first-class dtype (u8/u16/i32/f32/f64) encoded in the
//     type_id — see the Dtype block for the representation rationale.
//
// SPEC DEVIATION (recorded honestly): the agreed 32B DirEntry carried
// {key_hash, key_off, type_id, storage, union{inl|ext}, shape[3]} — that is
// 36+ bytes once key_len is included; shape[3] does not fit. Since the
// size-class pool ALREADY carries {type_id, shape[3], bytes} per EXTERN
// buffer (per the same spec), shape moved THERE: an EXTERN entry's shape is
// read off its buffer record, and INLINE entries (scalars/str/mp) have no
// shape by construction. The DirEntry stays exactly 32 bytes.
//
// Fail-loud contract: a typed read against the wrong stored type asserts in
// Debug and returns null/0 + a stderr warn (and bumps a counter) in Release.
// Tests flip SoftFailScope so the Debug build can exercise the Release
// behaviour without aborting.
//
// No xi_abi.h, no <windows.h>: std headers + <malloc.h> (_aligned_malloc).
//

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#else
#include <cstdlib>
#endif

namespace xi {
namespace packc {

// ===================================================================
// Handles + constants
// ===================================================================
using BufHandleC  = uint64_t;   // 0 = null; (gen << 16) | slot
using PackHandleC = uint64_t;   // 0 = null; (gen << 16) | slot

inline constexpr BufHandleC  kBufNull  = 0;
inline constexpr PackHandleC kPackNull = 0;

inline constexpr uint32_t kMagic   = 0x334B5058u;  // 'XPK3' little-endian
inline constexpr uint32_t kVersion = 1;

// Entry type ids. 1..0xFF reserved for the framework's own inline forms +
// the u8 tensor; >= 0x100 is the caller's typed-blob space (a blob's type_id
// is whatever the producer declares — the reader's as_blob(expect) checks it).
inline constexpr uint16_t kTypeI64      = 1;
inline constexpr uint16_t kTypeF64      = 2;
inline constexpr uint16_t kTypeStr      = 3;
inline constexpr uint16_t kTypeMp       = 4;   // opaque canonical msgpack bytes
inline constexpr uint16_t kTypeTensorU8 = 5;   // EXTERN, shape = {w,h,c}, u8
inline constexpr uint16_t kTypeTensorU16= 6;   // EXTERN tensor, u16 elements
inline constexpr uint16_t kTypeTensorI32= 7;   // EXTERN tensor, i32 elements
inline constexpr uint16_t kTypeTensorF32= 8;   // EXTERN tensor, f32 elements
inline constexpr uint16_t kTypeTensorF64= 9;   // EXTERN tensor, f64 elements
inline constexpr uint16_t kTypeUserBase = 0x100;

// ---- tensor dtype -------------------------------------------------
// REPRESENTATION CHOICE (documented per the dtype feature spec): dtype is
// encoded IN the type_id — one contiguous type_id per element type
// (kTypeTensorU8..kTypeTensorF64) rather than a single kTypeTensor + a
// separate dtype field. Rationale:
//   * type_id already travels everywhere shape does — the DirEntry, the
//     BufTable Slot, AND the 22-byte EXTERN wire record — so serialize/
//     deserialize round-trip the dtype with ZERO format change (the Slot has
//     spare room for a dtype u16, but a field that duplicates information the
//     type_id already carries would just be a second source of truth to keep
//     honest).
//   * as_tensor type-checking stays exactly as honest as as_blob: one u16
//     compare against the stored type_id.
// Dtype <-> type_id is a fixed offset (tensor_type_for / tensor_dtype).
enum Dtype : uint16_t {
    kDtypeU8  = 0,
    kDtypeU16 = 1,
    kDtypeI32 = 2,
    kDtypeF32 = 3,
    kDtypeF64 = 4,
};
inline constexpr uint32_t kDtypeElemSize[] = {1, 2, 4, 4, 8};
inline constexpr uint32_t dtype_elem_size(Dtype d) { return kDtypeElemSize[d]; }
inline constexpr bool is_tensor_type(uint16_t type_id) {
    return type_id >= kTypeTensorU8 && type_id <= kTypeTensorF64;
}
inline constexpr uint16_t tensor_type_for(Dtype d) {
    return uint16_t(kTypeTensorU8 + uint16_t(d));
}
inline constexpr Dtype tensor_dtype(uint16_t type_id) {   // pre: is_tensor_type
    return Dtype(type_id - kTypeTensorU8);
}
// C++ element type -> Dtype (drives as_tensor_of<T>). Unsupported T = no
// specialization = compile error, so a bogus element type can't slip through.
template <class T> struct dtype_of;                       // intentionally undefined
template <> struct dtype_of<uint8_t>  { static constexpr Dtype value = kDtypeU8;  };
template <> struct dtype_of<uint16_t> { static constexpr Dtype value = kDtypeU16; };
template <> struct dtype_of<int32_t>  { static constexpr Dtype value = kDtypeI32; };
template <> struct dtype_of<float>    { static constexpr Dtype value = kDtypeF32; };
template <> struct dtype_of<double>   { static constexpr Dtype value = kDtypeF64; };

// Storage discriminator.
inline constexpr uint16_t kStorageInline = 0;
inline constexpr uint16_t kStorageExtern = 1;

// ===================================================================
// Fail-loud plumbing. A type mismatch is a producer/consumer contract bug:
// Debug asserts (unless a test scoped SoftFail), Release warns once per call
// and returns the null value. The counter is the tests' oracle.
// ===================================================================
inline std::atomic<uint64_t>& type_mismatch_count() {
    static std::atomic<uint64_t> n{0};
    return n;
}
inline bool& soft_fail_flag() {
    thread_local bool f = false;
    return f;
}
// RAII: suppress the Debug assert so tests can drive the mismatch path.
struct SoftFailScope {
    bool prev;
    SoftFailScope() : prev(soft_fail_flag()) { soft_fail_flag() = true; }
    ~SoftFailScope() { soft_fail_flag() = prev; }
    SoftFailScope(const SoftFailScope&) = delete;
    SoftFailScope& operator=(const SoftFailScope&) = delete;
};
inline void fail_type(const char* what) {
    type_mismatch_count().fetch_add(1, std::memory_order_relaxed);
    std::fprintf(stderr, "[pack_c] type mismatch: %s\n", what);
    assert(soft_fail_flag() && "pack_c typed read against wrong stored type");
}

// ===================================================================
// FNV-1a 64 over the key bytes — the directory sort/search key. Collisions
// are handled (equal-hash runs are memcmp-verified), never assumed away.
// ===================================================================
inline uint64_t hash_key(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (char c : s) { h ^= uint8_t(c); h *= 1099511628211ull; }
    return h;
}

// ===================================================================
// Size-class raw allocator — 2^n classes from 4 KiB to 64 MiB, every
// allocation 64B-aligned (_aligned_malloc). Per-thread MAGAZINE (small LIFO,
// hottest-in-cache) with a global overflow shelf per class; a request over
// 64 MiB goes straight to the aligned heap (class 0xFF, never cached).
// ===================================================================
inline constexpr int kMinClassLog = 12;                        // 4 KiB
inline constexpr int kMaxClassLog = 26;                        // 64 MiB
inline constexpr int kNumClasses  = kMaxClassLog - kMinClassLog + 1;
inline constexpr uint8_t kClassDirect = 0xFF;                  // > 64 MiB
inline constexpr int kMagazineCap = 8;    // per thread per class
inline constexpr int kOverflowCap = 8;    // global per class

inline void* aligned_alloc64(size_t n) {
#ifdef _WIN32
    return _aligned_malloc(n, 64);
#else
    void* p = nullptr;
    if (posix_memalign(&p, 64, n) != 0) return nullptr;
    return p;
#endif
}
inline void aligned_free64(void* p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

inline uint8_t class_for(uint64_t bytes) {
    if (bytes == 0) bytes = 1;
    for (int c = kMinClassLog; c <= kMaxClassLog; ++c)
        if (bytes <= (uint64_t(1) << c)) return uint8_t(c - kMinClassLog);
    return kClassDirect;
}
inline uint64_t class_bytes(uint8_t cls) {
    return uint64_t(1) << (kMinClassLog + cls);
}

namespace detail {

// Global overflow shelf: one mutex-guarded stack per class. Cold path only —
// hit when a magazine over/underflows or a thread exits with a loaded
// magazine. Intentionally leaked (process-lifetime, like ImagePool).
struct OverflowShelf {
    std::mutex mu;
    std::vector<void*> free_[kNumClasses];

    static OverflowShelf& instance() {
        static OverflowShelf* s = new OverflowShelf();
        return *s;
    }
    void* pop(uint8_t cls) {
        std::lock_guard<std::mutex> lk(mu);
        auto& v = free_[cls];
        if (v.empty()) return nullptr;
        void* p = v.back();
        v.pop_back();
        return p;
    }
    // Returns false when the shelf is full (caller frees outright).
    bool push(uint8_t cls, void* p) {
        std::lock_guard<std::mutex> lk(mu);
        auto& v = free_[cls];
        if ((int)v.size() >= kOverflowCap) return false;
        v.push_back(p);
        return true;
    }
};

// Per-thread magazines. The destructor drains survivors to the shelf (or the
// heap) so a dying thread never leaks its cached slabs.
struct MagazineSet {
    void* items[kNumClasses][kMagazineCap];
    int   count[kNumClasses];

    MagazineSet() { std::memset(count, 0, sizeof count); }
    ~MagazineSet() {
        for (int c = 0; c < kNumClasses; ++c)
            for (int i = 0; i < count[c]; ++i)
                if (!OverflowShelf::instance().push(uint8_t(c), items[c][i]))
                    aligned_free64(items[c][i]);
    }
};
inline MagazineSet& magazines() {
    thread_local MagazineSet m;
    return m;
}

} // namespace detail

// Raw class alloc/free — used by pooled EXTERN buffers AND pack slabs (a slab
// "returns to its size class" through the identical path).
inline void* raw_class_alloc(uint64_t bytes, uint8_t* cls_out) {
    uint8_t cls = class_for(bytes);
    *cls_out = cls;
    if (cls == kClassDirect) return aligned_alloc64(bytes);
    auto& mag = detail::magazines();
    if (mag.count[cls] > 0) return mag.items[cls][--mag.count[cls]];   // hot
    if (void* p = detail::OverflowShelf::instance().pop(cls)) return p;
    return aligned_alloc64(class_bytes(cls));
}
inline void raw_class_free(void* p, uint8_t cls) {
    if (!p) return;
    if (cls == kClassDirect) { aligned_free64(p); return; }
    auto& mag = detail::magazines();
    if (mag.count[cls] < kMagazineCap) { mag.items[cls][mag.count[cls]++] = p; return; }
    if (!detail::OverflowShelf::instance().push(cls, p)) aligned_free64(p);
}

// ===================================================================
// BufTable — the EXTERN typed-buffer handle table (ImagePool discipline).
//
// Fixed slot array; a handle packs (generation << 16) | slot. Resolve is a
// lock-free generation check against the slot's live_gen (0 = free), so a
// stale handle held past release fails cleanly instead of aliasing the next
// occupant. Unlike ImagePool there is NO heap PoolEntry per mint: the record
// lives in the slot itself — that per-create allocation is one of the costs
// design C exists to remove.
// ===================================================================
class BufTable {
public:
    static constexpr uint32_t SLOT_BITS  = 16;
    static constexpr uint32_t SLOT_COUNT = 1u << SLOT_BITS;
    static constexpr uint64_t SLOT_MASK  = SLOT_COUNT - 1;
    static constexpr uint64_t GEN_MAX    = (uint64_t(1) << 48) - 1;

    // Intentionally leaked (process-lifetime), mirroring ImagePool::instance:
    // a release during static teardown is unconditionally safe.
    static BufTable& instance() {
        static BufTable* t = new BufTable();
        return *t;
    }

    // Mint a refcounted buffer (rc = 1) of `bytes` from the size-class pool
    // and copy `src` in (src may be null = uninitialized). 0 on exhaustion.
    BufHandleC mint(uint16_t type_id, const uint32_t shape[3], uint64_t bytes,
                    const void* src) {
        if (bytes == 0) return kBufNull;
        uint8_t cls = 0;
        void* mem = raw_class_alloc(bytes, &cls);
        if (!mem) return kBufNull;
        uint32_t idx = acquire_slot_();
        if (idx == UINT32_MAX) {
            raw_class_free(mem, cls);
            std::fprintf(stderr, "[pack_c] BufTable exhausted (cap=%u)\n", SLOT_COUNT);
            return kBufNull;
        }
        Slot& s = slots_[idx];
        s.mem     = static_cast<uint8_t*>(mem);
        s.bytes   = bytes;
        s.cls     = cls;
        s.type_id = type_id;
        s.shape[0] = shape ? shape[0] : 0;
        s.shape[1] = shape ? shape[1] : 0;
        s.shape[2] = shape ? shape[2] : 0;
        s.rc.store(1, std::memory_order_relaxed);
        if (src) std::memcpy(mem, src, bytes);
        uint64_t gen = (s.gen_counter.fetch_add(1, std::memory_order_relaxed) + 1) & GEN_MAX;
        s.live_gen.store(gen, std::memory_order_release);      // publish
        return (gen << SLOT_BITS) | idx;
    }

    void addref(BufHandleC h) {
        if (Slot* s = resolve_(h)) s->rc.fetch_add(1, std::memory_order_relaxed);
    }
    void release(BufHandleC h) {
        Slot* s = resolve_(h);
        if (!s) return;
        if (s->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            s->live_gen.store(0, std::memory_order_release);   // retire the gen
            raw_class_free(s->mem, s->cls);
            s->mem = nullptr;
            release_slot_(uint32_t(h & SLOT_MASK));
        }
    }

    // Lock-free reads. Caller contract (same as ImagePool): hold a ref while
    // touching the bytes, or a concurrent release can retire the slot.
    const uint8_t* data(BufHandleC h)   { Slot* s = resolve_(h); return s ? s->mem : nullptr; }
    uint8_t* writable_data(BufHandleC h){ Slot* s = resolve_(h); return s ? s->mem : nullptr; }
    uint64_t bytes(BufHandleC h)        { Slot* s = resolve_(h); return s ? s->bytes : 0; }
    uint16_t type_id(BufHandleC h)      { Slot* s = resolve_(h); return s ? s->type_id : 0; }
    uint32_t shape(BufHandleC h, int i) {
        Slot* s = resolve_(h);
        return (s && i >= 0 && i < 3) ? s->shape[i] : 0;
    }
    int32_t refcount(BufHandleC h) {    // diagnostic (tests' rc oracle)
        Slot* s = resolve_(h);
        return s ? s->rc.load(std::memory_order_relaxed) : 0;
    }
    // Diagnostic: live buffer count (leak oracle for the tests).
    int32_t live() const { return live_count_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<uint64_t> live_gen{0};     // 0 = free; else current gen
        std::atomic<uint64_t> gen_counter{0};  // monotonic reuse counter
        std::atomic<int32_t>  rc{0};
        std::atomic<uint32_t> next_free{0};    // free-list link (entry free)
        uint8_t*  mem = nullptr;
        uint64_t  bytes = 0;
        uint8_t   cls = 0;
        uint16_t  type_id = 0;
        uint32_t  shape[3] = {0, 0, 0};
    };

    Slot* resolve_(BufHandleC h) {
        if (h == kBufNull) return nullptr;
        Slot& s = slots_[uint32_t(h & SLOT_MASK)];
        uint64_t lg = s.live_gen.load(std::memory_order_acquire);
        if (lg == 0 || lg != ((h >> SLOT_BITS) & GEN_MAX)) return nullptr;
        return &s;
    }

    // Treiber free list, version-packed head against ABA (ImagePool's shape).
    uint32_t acquire_slot_() {
        while (true) {
            uint64_t head = free_head_.load(std::memory_order_acquire);
            uint32_t idx = uint32_t(head & 0xFFFFFFFFu);
            if (idx == 0) break;
            uint32_t next = slots_[idx].next_free.load(std::memory_order_relaxed);
            uint64_t new_head = (((head >> 32) + 1) << 32) | next;
            if (free_head_.compare_exchange_weak(head, new_head,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                live_count_.fetch_add(1, std::memory_order_relaxed);
                return idx;
            }
        }
        uint32_t idx = next_fresh_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= SLOT_COUNT) {           // saturate, never wrap onto live slots
            next_fresh_.store(SLOT_COUNT, std::memory_order_relaxed);
            return UINT32_MAX;
        }
        live_count_.fetch_add(1, std::memory_order_relaxed);
        return idx;
    }
    void release_slot_(uint32_t idx) {
        live_count_.fetch_sub(1, std::memory_order_relaxed);
        while (true) {
            uint64_t head = free_head_.load(std::memory_order_acquire);
            slots_[idx].next_free.store(uint32_t(head & 0xFFFFFFFFu),
                                        std::memory_order_relaxed);
            uint64_t new_head = (((head >> 32) + 1) << 32) | idx;
            if (free_head_.compare_exchange_weak(head, new_head,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return;
        }
    }

    std::unique_ptr<Slot[]> slots_{new Slot[SLOT_COUNT]};
    std::atomic<uint32_t> next_fresh_{1};   // slot 0 reserved (handle 0 = null)
    std::atomic<uint64_t> free_head_{0};
    std::atomic<int32_t>  live_count_{0};
};

// ===================================================================
// Slab layout structs — POD, memcpy'd in and out of the slab.
// ===================================================================
struct PackHeaderC {                 // 64 bytes exactly
    uint32_t magic;                  // 'XPK3'
    uint32_t version;
    uint32_t flags;
    uint32_t entry_count;
    uint32_t dir_offset;             // == 64
    uint32_t payload_offset;         // == 64 + 32 * entry_count (8-aligned)
    uint64_t slab_bytes;             // logical slab size (<= class size)
    uint64_t pack_id;                // monotonic mint counter
    int64_t  ts_us;
    uint8_t  pad[16];
};
static_assert(sizeof(PackHeaderC) == 64, "PackHeaderC must be 64 bytes");

struct DirEntryC {                   // 32 bytes exactly (see header comment
    uint64_t key_hash;               // for the shape[3] relocation note)
    uint32_t key_off;                // slab-relative offset of the key bytes
    uint32_t key_len;
    uint16_t type_id;
    uint16_t storage;                // kStorageInline | kStorageExtern
    uint32_t reserved;               // 0 (kept for the 32B budget's honesty)
    union {
        struct { uint32_t off; uint32_t len; } inl;   // slab-relative payload
        uint64_t ext;                                 // BufHandleC
    } u;
};
static_assert(sizeof(DirEntryC) == 32, "DirEntryC must be 32 bytes");

// ===================================================================
// PackTableC — slot+generation handle table for sealed packs.
//
// retain/release = one atomic rc on the slot. Last release walks the slab's
// directory, releases every EXTERN child ref, and returns the slab to its
// size class — the whole destroy is table-local (no registry map, no lock).
// ===================================================================
class PackTableC {
public:
    static constexpr uint32_t SLOT_BITS  = 16;
    static constexpr uint32_t SLOT_COUNT = 1u << SLOT_BITS;
    static constexpr uint64_t SLOT_MASK  = SLOT_COUNT - 1;
    static constexpr uint64_t GEN_MAX    = (uint64_t(1) << 48) - 1;

    static PackTableC& instance() {      // intentionally leaked, as above
        static PackTableC* t = new PackTableC();
        return *t;
    }

    // Adopt a fully-written slab (ownership transfers; rc = 1).
    PackHandleC mint(uint8_t* slab, uint8_t slab_cls) {
        uint32_t idx = acquire_slot_();
        if (idx == UINT32_MAX) {
            destroy_slab_(slab, slab_cls);
            std::fprintf(stderr, "[pack_c] PackTableC exhausted (cap=%u)\n", SLOT_COUNT);
            return kPackNull;
        }
        Slot& s = slots_[idx];
        s.slab = slab;
        s.cls  = slab_cls;
        s.rc.store(1, std::memory_order_relaxed);
        uint64_t gen = (s.gen_counter.fetch_add(1, std::memory_order_relaxed) + 1) & GEN_MAX;
        s.live_gen.store(gen, std::memory_order_release);
        return (gen << SLOT_BITS) | idx;
    }

    void retain(PackHandleC h) {
        if (Slot* s = resolve_(h)) s->rc.fetch_add(1, std::memory_order_relaxed);
    }
    void release(PackHandleC h) {
        Slot* s = resolve_(h);
        if (!s) return;
        if (s->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            s->live_gen.store(0, std::memory_order_release);
            destroy_slab_(s->slab, s->cls);
            s->slab = nullptr;
            release_slot_(uint32_t(h & SLOT_MASK));
        }
    }

    // Lock-free resolve to the slab base. Caller contract: hold a ref.
    const uint8_t* slab(PackHandleC h) {
        Slot* s = resolve_(h);
        return s ? s->slab : nullptr;
    }
    int32_t refcount(PackHandleC h) {    // diagnostic
        Slot* s = resolve_(h);
        return s ? s->rc.load(std::memory_order_relaxed) : 0;
    }
    int32_t live() const { return live_count_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<uint64_t> live_gen{0};
        std::atomic<uint64_t> gen_counter{0};
        std::atomic<int32_t>  rc{0};
        std::atomic<uint32_t> next_free{0};
        uint8_t* slab = nullptr;
        uint8_t  cls = 0;
    };

    // Destroy = dir walk (release EXTERN children) + slab back to its class.
    static void destroy_slab_(uint8_t* slab, uint8_t cls) {
        if (!slab) return;
        PackHeaderC hd;
        std::memcpy(&hd, slab, sizeof hd);
        const auto* dir = reinterpret_cast<const DirEntryC*>(slab + hd.dir_offset);
        for (uint32_t i = 0; i < hd.entry_count; ++i)
            if (dir[i].storage == kStorageExtern)
                BufTable::instance().release(dir[i].u.ext);
        raw_class_free(slab, cls);
    }

    Slot* resolve_(PackHandleC h) {
        if (h == kPackNull) return nullptr;
        Slot& s = slots_[uint32_t(h & SLOT_MASK)];
        uint64_t lg = s.live_gen.load(std::memory_order_acquire);
        if (lg == 0 || lg != ((h >> SLOT_BITS) & GEN_MAX)) return nullptr;
        return &s;
    }
    uint32_t acquire_slot_() {
        while (true) {
            uint64_t head = free_head_.load(std::memory_order_acquire);
            uint32_t idx = uint32_t(head & 0xFFFFFFFFu);
            if (idx == 0) break;
            uint32_t next = slots_[idx].next_free.load(std::memory_order_relaxed);
            uint64_t new_head = (((head >> 32) + 1) << 32) | next;
            if (free_head_.compare_exchange_weak(head, new_head,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                live_count_.fetch_add(1, std::memory_order_relaxed);
                return idx;
            }
        }
        uint32_t idx = next_fresh_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= SLOT_COUNT) {
            next_fresh_.store(SLOT_COUNT, std::memory_order_relaxed);
            return UINT32_MAX;
        }
        live_count_.fetch_add(1, std::memory_order_relaxed);
        return idx;
    }
    void release_slot_(uint32_t idx) {
        live_count_.fetch_sub(1, std::memory_order_relaxed);
        while (true) {
            uint64_t head = free_head_.load(std::memory_order_acquire);
            slots_[idx].next_free.store(uint32_t(head & 0xFFFFFFFFu),
                                        std::memory_order_relaxed);
            uint64_t new_head = (((head >> 32) + 1) << 32) | idx;
            if (free_head_.compare_exchange_weak(head, new_head,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return;
        }
    }

    std::unique_ptr<Slot[]> slots_{new Slot[SLOT_COUNT]};
    std::atomic<uint32_t> next_fresh_{1};
    std::atomic<uint64_t> free_head_{0};
    std::atomic<int32_t>  live_count_{0};
};

// ===================================================================
// RAII refs — the C++-side DEFAULT ownership vocabulary.
//
// The raw-handle APIs (BufHandleC/PackHandleC + addref/retain/release) stay:
// they model the C-ABI seam — the wire shape a plugin sees across the DLL
// boundary. C++ users hold PackRef/BufRef instead and never touch a manual
// retain/release; the normal flow is
//
//     PackRef pk = builder.seal_ref();          // owns the seal's rc=1
//     PackViewC pak(pk);                        // borrow for reading
//     BufRef buf = pak["img"].buf_ref();        // co-own a buffer
//
// Discipline (identical for both classes):
//   * raw-handle ctor ADOPTS — it takes over an existing ref (no extra
//     retain), so wrapping the rc=1 a mint/seal returns is exactly right;
//   * copy = retain/addref, move = steal, dtor/reset = release;
//   * get() exposes the raw handle for the C-ABI seam (non-owning peek).
// ===================================================================
class BufRef {
public:
    BufRef() = default;
    // ADOPTING: takes over one existing ref (e.g. the rc=1 from mint()).
    // To co-own a buffer you don't own a ref on, use BufRef::retained().
    explicit BufRef(BufHandleC h) noexcept : h_(h) {}
    static BufRef retained(BufHandleC h) {              // addref'd copy
        if (h != kBufNull) BufTable::instance().addref(h);
        return BufRef(h);
    }
    BufRef(const BufRef& o) : h_(o.h_) {
        if (h_ != kBufNull) BufTable::instance().addref(h_);
    }
    BufRef(BufRef&& o) noexcept : h_(o.h_) { o.h_ = kBufNull; }
    BufRef& operator=(const BufRef& o) {
        if (this != &o) { BufRef tmp(o); std::swap(h_, tmp.h_); }
        return *this;
    }
    BufRef& operator=(BufRef&& o) noexcept {
        if (this != &o) { reset(); h_ = o.h_; o.h_ = kBufNull; }
        return *this;
    }
    ~BufRef() { reset(); }

    BufHandleC get() const { return h_; }
    explicit operator bool() const { return h_ != kBufNull; }
    void reset() {
        if (h_ != kBufNull) { BufTable::instance().release(h_); h_ = kBufNull; }
    }

private:
    BufHandleC h_ = kBufNull;
};

class PackRef {
public:
    PackRef() = default;
    // ADOPTING: takes over one existing ref (e.g. the rc=1 from seal()) —
    // no extra retain. To co-own use PackRef::retained().
    explicit PackRef(PackHandleC h) noexcept : h_(h) {}
    static PackRef retained(PackHandleC h) {
        if (h != kPackNull) PackTableC::instance().retain(h);
        return PackRef(h);
    }
    PackRef(const PackRef& o) : h_(o.h_) {
        if (h_ != kPackNull) PackTableC::instance().retain(h_);
    }
    PackRef(PackRef&& o) noexcept : h_(o.h_) { o.h_ = kPackNull; }
    PackRef& operator=(const PackRef& o) {
        if (this != &o) { PackRef tmp(o); std::swap(h_, tmp.h_); }
        return *this;
    }
    PackRef& operator=(PackRef&& o) noexcept {
        if (this != &o) { reset(); h_ = o.h_; o.h_ = kPackNull; }
        return *this;
    }
    ~PackRef() { reset(); }

    PackHandleC get() const { return h_; }
    explicit operator bool() const { return h_ != kPackNull; }
    void reset() {
        if (h_ != kPackNull) { PackTableC::instance().release(h_); h_ = kPackNull; }
    }

private:
    PackHandleC h_ = kPackNull;
};

// ===================================================================
// Read side — EntryViewC / PackViewC.
//
// A PackViewC is a borrowed, trivially-copyable view over the slab of a pack
// the CALLER holds a ref on (the discipline every handle table here shares).
// Lookup is a binary search on the hash-sorted directory; an equal-hash run
// is memcmp-verified against the actual key bytes.
// ===================================================================
struct TensorViewC {
    const uint8_t* data = nullptr;
    uint64_t bytes = 0;
    uint32_t shape[3] = {0, 0, 0};
    uint16_t type_id = 0;
    Dtype    dtype = kDtypeU8;
    uint32_t elem_size = 0;          // bytes per element (dtype table)
    bool ok() const { return data != nullptr; }
};

// Element-typed tensor view — what as_tensor_of<T>() hands back: the same
// buffer, but `data` already carries the element type and `count` is in
// ELEMENTS, so the caller never reinterpret_casts.
template <class T>
struct TensorOfC {
    const T* data = nullptr;
    uint64_t count = 0;              // elements, not bytes
    uint32_t shape[3] = {0, 0, 0};
    bool ok() const { return data != nullptr; }
};

class EntryViewC {
public:
    EntryViewC() = default;
    EntryViewC(const uint8_t* slab, const DirEntryC* e) : slab_(slab), e_(e) {}

    bool valid() const { return e_ != nullptr; }
    uint16_t type() const { return e_ ? e_->type_id : 0; }
    uint16_t storage() const { return e_ ? e_->storage : 0; }
    std::string_view key() const {
        if (!e_) return {};
        return std::string_view(reinterpret_cast<const char*>(slab_ + e_->key_off),
                                e_->key_len);
    }
    // EXTERN shape from the buffer record; INLINE entries report {0,0,0}
    // (see the header's shape-relocation note).
    uint32_t shape(int i) const {
        if (!e_ || e_->storage != kStorageExtern) return 0;
        return BufTable::instance().shape(e_->u.ext, i);
    }

    // ---- typed reads (fail-loud on mismatch) --------------------------
    int64_t as_i64() const {
        if (!check_(kTypeI64, "as_i64")) return 0;
        int64_t v;
        std::memcpy(&v, slab_ + e_->u.inl.off, 8);   // RAW: one 8-byte load
        return v;
    }
    double as_f64() const {
        if (!check_(kTypeF64, "as_f64")) return 0.0;
        double v;
        std::memcpy(&v, slab_ + e_->u.inl.off, 8);
        return v;
    }
    std::string_view as_str() const {
        if (!check_(kTypeStr, "as_str")) return {};
        return std::string_view(reinterpret_cast<const char*>(slab_ + e_->u.inl.off),
                                e_->u.inl.len);
    }
    std::span<const uint8_t> as_mp() const {
        if (!check_(kTypeMp, "as_mp")) return {};
        return std::span<const uint8_t>(slab_ + e_->u.inl.off, e_->u.inl.len);
    }
    // Any-dtype tensor view (the dtype travels in the type_id — see the
    // Dtype block up top). Fail-loud on a non-tensor entry.
    TensorViewC as_tensor() const {
        TensorViewC t;
        if (!e_ || !is_tensor_type(e_->type_id) || e_->storage != kStorageExtern) {
            if (e_) fail_type("as_tensor");
            return t;
        }
        auto& bt = BufTable::instance();
        t.data    = bt.data(e_->u.ext);
        t.bytes   = bt.bytes(e_->u.ext);
        t.shape[0] = bt.shape(e_->u.ext, 0);
        t.shape[1] = bt.shape(e_->u.ext, 1);
        t.shape[2] = bt.shape(e_->u.ext, 2);
        t.type_id = e_->type_id;
        t.dtype   = tensor_dtype(e_->type_id);
        t.elem_size = dtype_elem_size(t.dtype);
        return t;
    }
    // Element-typed read: fail-loud unless the STORED dtype matches T —
    // the same producer/consumer contract discipline as as_blob(expect).
    template <class T>
    TensorOfC<T> as_tensor_of() const {
        constexpr Dtype want = dtype_of<T>::value;
        TensorOfC<T> t;
        if (!e_ || e_->storage != kStorageExtern ||
            e_->type_id != tensor_type_for(want)) {
            if (e_) fail_type("as_tensor_of");
            return t;
        }
        auto& bt = BufTable::instance();
        t.data  = reinterpret_cast<const T*>(bt.data(e_->u.ext));
        t.count = bt.bytes(e_->u.ext) / sizeof(T);
        t.shape[0] = bt.shape(e_->u.ext, 0);
        t.shape[1] = bt.shape(e_->u.ext, 1);
        t.shape[2] = bt.shape(e_->u.ext, 2);
        return t;
    }
    std::span<const uint8_t> as_blob(uint16_t expect_type_id) const {
        if (!e_ || e_->storage != kStorageExtern || e_->type_id != expect_type_id) {
            if (e_) fail_type("as_blob");
            return {};
        }
        auto& bt = BufTable::instance();
        const uint8_t* p = bt.data(e_->u.ext);
        if (!p) return {};
        return std::span<const uint8_t>(p, bt.bytes(e_->u.ext));
    }
    // The raw EXTERN handle (the C-ABI seam; NOT owning — pair with a manual
    // addref if you keep it past the pack's life).
    BufHandleC buf_handle() const {
        return (e_ && e_->storage == kStorageExtern) ? e_->u.ext : kBufNull;
    }
    // Owning RAII co-ref on the EXTERN buffer (addref'd): the C++-side way
    // to keep the bytes alive past every pack that carries them.
    BufRef buf_ref() const { return BufRef::retained(buf_handle()); }

private:
    bool check_(uint16_t want, const char* what) const {
        if (!e_) return false;                       // missing key = quiet null
        if (e_->type_id != want || e_->storage != kStorageInline) {
            fail_type(what);
            return false;
        }
        return true;
    }
    const uint8_t* slab_ = nullptr;
    const DirEntryC* e_ = nullptr;
};

class PackViewC {
public:
    PackViewC() = default;
    explicit PackViewC(PackHandleC h) : slab_(PackTableC::instance().slab(h)) {}
    // Borrow from a PackRef (the RAII default). The view does NOT take a ref:
    // the caller's PackRef must outlive the view, same contract as the raw form.
    explicit PackViewC(const PackRef& r) : PackViewC(r.get()) {}
    explicit PackViewC(const uint8_t* slab) : slab_(slab) {}

    bool valid() const { return slab_ != nullptr; }
    const PackHeaderC* header() const {
        return reinterpret_cast<const PackHeaderC*>(slab_);
    }
    uint32_t size() const { return slab_ ? header()->entry_count : 0; }

    EntryViewC operator[](std::string_view key) const { return find(key); }
    EntryViewC find(std::string_view key) const {
        if (!slab_) return {};
        const PackHeaderC* hd = header();
        const auto* dir = reinterpret_cast<const DirEntryC*>(slab_ + hd->dir_offset);
        const uint64_t h = hash_key(key);
        // Binary search on the hash, then memcmp-verify the equal-hash run.
        uint32_t lo = 0, hi = hd->entry_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (dir[mid].key_hash < h) lo = mid + 1; else hi = mid;
        }
        for (; lo < hd->entry_count && dir[lo].key_hash == h; ++lo) {
            const DirEntryC& e = dir[lo];
            if (e.key_len == key.size() &&
                std::memcmp(slab_ + e.key_off, key.data(), key.size()) == 0)
                return EntryViewC(slab_, &e);
        }
        return {};
    }

    // Directory-order iteration (hash order, NOT insertion order — a design-C
    // property worth knowing before any migration: insertion order is lost).
    EntryViewC at(uint32_t i) const {
        if (!slab_ || i >= size()) return {};
        const auto* dir = reinterpret_cast<const DirEntryC*>(slab_ + header()->dir_offset);
        return EntryViewC(slab_, &dir[i]);
    }
    const uint8_t* slab() const { return slab_; }

private:
    const uint8_t* slab_ = nullptr;
};

// ===================================================================
// PackBuilderC — collect entries, bump the payload, sort, write, seal.
//
// Staging (payload bytes + entry rows) lives in a per-thread SCRATCH recycle,
// so a steady stream of builds on one thread is heap-free after warmup — the
// same discipline the current ArenaPool gives PackBuilder, kept equivalent so
// the bench compares representations, not allocator hygiene.
// ===================================================================
namespace detail {

struct TmpEntry {
    uint64_t hash;
    uint32_t key_off, key_len;       // payload-relative (rebased at seal)
    uint16_t type_id, storage;
    uint32_t inl_off, inl_len;       // payload-relative
    uint64_t ext;
};

struct BuilderScratch {
    std::vector<uint8_t>  payload;
    std::vector<TmpEntry> entries;
    void clear() { payload.clear(); entries.clear(); }   // capacity kept
};

// ONE pool function, shared by get and put — two separate thread_locals here
// would silently defeat the recycle (every put would feed a pool no get reads)
// and every build would heap-allocate again.
inline std::vector<std::unique_ptr<BuilderScratch>>& scratch_pool_() {
    thread_local std::vector<std::unique_ptr<BuilderScratch>> pool;
    return pool;
}
inline BuilderScratch* scratch_get() {
    auto& pool = scratch_pool_();
    if (!pool.empty()) {
        BuilderScratch* s = pool.back().release();
        pool.pop_back();
        return s;
    }
    return new BuilderScratch();
}
inline void scratch_put(BuilderScratch* s) {
    s->clear();
    auto& pool = scratch_pool_();
    if (pool.size() < 8) pool.emplace_back(s);
    else delete s;
}
} // namespace detail

class PackBuilderC {
public:
    PackBuilderC() : s_(detail::scratch_get()) {}
    ~PackBuilderC() {
        if (!s_) return;
        // Abandoned without seal: release every EXTERN ref this builder minted
        // or adopted (mirrors PackBuilder's abandoned-builder contract).
        for (const auto& e : s_->entries)
            if (e.storage == kStorageExtern) BufTable::instance().release(e.ext);
        detail::scratch_put(s_);
    }
    PackBuilderC(const PackBuilderC&) = delete;
    PackBuilderC& operator=(const PackBuilderC&) = delete;

    void add_i64(std::string_view key, int64_t v) {
        detail::TmpEntry e = begin_(key, kTypeI64);
        e.inl_off = bump_(8, 8);
        e.inl_len = 8;
        std::memcpy(s_->payload.data() + e.inl_off, &v, 8);
        s_->entries.push_back(e);
    }
    void add_f64(std::string_view key, double v) {
        detail::TmpEntry e = begin_(key, kTypeF64);
        e.inl_off = bump_(8, 8);
        e.inl_len = 8;
        std::memcpy(s_->payload.data() + e.inl_off, &v, 8);
        s_->entries.push_back(e);
    }
    void add_str(std::string_view key, std::string_view v) {
        detail::TmpEntry e = begin_(key, kTypeStr);
        e.inl_off = bump_(uint32_t(v.size()), 1);
        e.inl_len = uint32_t(v.size());
        if (!v.empty()) std::memcpy(s_->payload.data() + e.inl_off, v.data(), v.size());
        s_->entries.push_back(e);
    }
    // Opaque canonical msgpack bytes (caller encodes with xi::mp::Writer).
    void add_mp(std::string_view key, const void* mp, size_t n) {
        detail::TmpEntry e = begin_(key, kTypeMp);
        e.inl_off = bump_(uint32_t(n), 8);
        e.inl_len = uint32_t(n);
        if (n) std::memcpy(s_->payload.data() + e.inl_off, mp, n);
        s_->entries.push_back(e);
    }
    // Tensor: elements copied into a pooled 64B-aligned buffer, shape {w,h,c},
    // bytes = w*h*c*elem_size(dtype). The dtype rides in the type_id.
    bool add_tensor(std::string_view key, uint32_t w, uint32_t h, uint32_t c,
                    Dtype dtype, const void* px) {
        const uint32_t shape[3] = {w, h, c};
        uint64_t bytes = uint64_t(w) * h * c * dtype_elem_size(dtype);
        BufHandleC bh = BufTable::instance().mint(tensor_type_for(dtype), shape,
                                                  bytes, px);
        if (bh == kBufNull) return false;
        push_extern_(key, tensor_type_for(dtype), bh);
        return true;
    }
    // u8 convenience (the original signature — image-shaped callers).
    bool add_tensor(std::string_view key, uint32_t w, uint32_t h, uint32_t c,
                    const void* px) {
        return add_tensor(key, w, h, c, kDtypeU8, px);
    }
    // Typed blob: caller-declared type_id (>= kTypeUserBase), bytes copied.
    bool add_blob(std::string_view key, uint16_t type_id, const void* data,
                  uint64_t n) {
        const uint32_t shape[3] = {uint32_t(n), 1, 1};
        BufHandleC bh = BufTable::instance().mint(type_id, shape, n, data);
        if (bh == kBufNull) return false;
        push_extern_(key, type_id, bh);
        return true;
    }
    // Adopt an existing pooled buffer (zero-copy cross-pack share): addref —
    // this pack becomes a co-owner, releasing its ref on destroy.
    bool adopt(std::string_view key, BufHandleC bh) {
        auto& bt = BufTable::instance();
        uint16_t tid = bt.type_id(bh);
        if (!bt.data(bh)) return false;      // dead/stale handle: refuse
        bt.addref(bh);
        push_extern_(key, tid, bh);
        return true;
    }
    // RAII adopt: the pack takes its OWN ref; the caller's BufRef is untouched
    // (still owning, still valid).
    bool adopt(std::string_view key, const BufRef& br) {
        return adopt(key, br.get());
    }

    // RAII seal — the C++-side default: the returned PackRef adopts the
    // seal's rc=1, so the normal flow never touches a raw handle. seal()
    // (below) stays as the raw form because the raw handle IS the C-ABI
    // wire shape (and the bench drives it directly); this keeps ONE seal
    // implementation instead of a seal/seal_raw split.
    PackRef seal_ref(int64_t ts_us = 0) { return PackRef(seal(ts_us)); }

    // Sort the directory, write the slab, hand it to the pack table (rc = 1).
    // The builder is spent afterwards (scratch returned).
    PackHandleC seal(int64_t ts_us = 0) {
        assert(s_ && "double seal");
        auto& sc = *s_;
        const uint32_t n = uint32_t(sc.entries.size());
        const uint32_t dir_off = uint32_t(sizeof(PackHeaderC));
        const uint32_t payload_off = dir_off + n * uint32_t(sizeof(DirEntryC));
        const uint64_t slab_bytes = payload_off + sc.payload.size();

        uint8_t cls = 0;
        uint8_t* slab = static_cast<uint8_t*>(raw_class_alloc(slab_bytes, &cls));
        if (!slab) { abandon_(); return kPackNull; }

        // Directory order: (key_hash, key bytes) — deterministic, binary-
        // searchable, collision runs adjacent.
        const uint8_t* pay = sc.payload.data();
        std::sort(sc.entries.begin(), sc.entries.end(),
                  [pay](const detail::TmpEntry& a, const detail::TmpEntry& b) {
                      if (a.hash != b.hash) return a.hash < b.hash;
                      int c = std::memcmp(pay + a.key_off, pay + b.key_off,
                                          a.key_len < b.key_len ? a.key_len : b.key_len);
                      if (c != 0) return c < 0;
                      return a.key_len < b.key_len;
                  });

        PackHeaderC hd{};
        hd.magic          = kMagic;
        hd.version        = kVersion;
        hd.flags          = 0;
        hd.entry_count    = n;
        hd.dir_offset     = dir_off;
        hd.payload_offset = payload_off;
        hd.slab_bytes     = slab_bytes;
        hd.pack_id        = next_pack_id_().fetch_add(1, std::memory_order_relaxed);
        hd.ts_us          = ts_us;
        std::memcpy(slab, &hd, sizeof hd);

        auto* dir = reinterpret_cast<DirEntryC*>(slab + dir_off);
        for (uint32_t i = 0; i < n; ++i) {
            const detail::TmpEntry& t = sc.entries[i];
            DirEntryC d{};
            d.key_hash = t.hash;
            d.key_off  = t.key_off + payload_off;    // rebase payload -> slab
            d.key_len  = t.key_len;
            d.type_id  = t.type_id;
            d.storage  = t.storage;
            d.reserved = 0;
            if (t.storage == kStorageExtern) d.u.ext = t.ext;
            else { d.u.inl.off = t.inl_off + payload_off; d.u.inl.len = t.inl_len; }
            std::memcpy(&dir[i], &d, sizeof d);
        }
        if (!sc.payload.empty())
            std::memcpy(slab + payload_off, sc.payload.data(), sc.payload.size());

        // EXTERN refs now belong to the pack (its destroy walks the dir).
        sc.entries.clear();
        detail::scratch_put(s_);
        s_ = nullptr;
        return PackTableC::instance().mint(slab, cls);
    }

private:
    static std::atomic<uint64_t>& next_pack_id_() {
        static std::atomic<uint64_t> id{1};
        return id;
    }
    detail::TmpEntry begin_(std::string_view key, uint16_t type_id) {
        assert(s_ && "add after seal");
        detail::TmpEntry e{};
        e.hash    = hash_key(key);
        e.key_off = bump_(uint32_t(key.size()), 1);
        e.key_len = uint32_t(key.size());
        if (!key.empty())
            std::memcpy(s_->payload.data() + e.key_off, key.data(), key.size());
        e.type_id = type_id;
        e.storage = kStorageInline;
        return e;
    }
    void push_extern_(std::string_view key, uint16_t type_id, BufHandleC bh) {
        detail::TmpEntry e = begin_(key, type_id);
        e.storage = kStorageExtern;
        e.ext = bh;
        s_->entries.push_back(e);
    }
    // Bump-allocate n payload bytes at `align`; returns payload-relative off.
    // payload_offset is 8-aligned in the slab (64 + 32*n), so payload-relative
    // alignment survives the rebase for align <= 8.
    uint32_t bump_(uint32_t n, uint32_t align) {
        size_t off = (s_->payload.size() + (align - 1)) & ~size_t(align - 1);
        s_->payload.resize(off + n);
        return uint32_t(off);
    }
    void abandon_() {
        for (const auto& e : s_->entries)
            if (e.storage == kStorageExtern) BufTable::instance().release(e.ext);
        detail::scratch_put(s_);
        s_ = nullptr;
    }

    detail::BuilderScratch* s_;
};

// ===================================================================
// Serialization.
//
//   wire = slab[slab_bytes] verbatim
//        ++ for each EXTERN dir entry, in directory order:
//             u16 type_id, u32 shape[3], u64 bytes, raw bytes
//
// The slab's ext handles travel as-is (they are meaningless off-process);
// deserialize re-mints each buffer into the local pool and PATCHES the fresh
// slab copy's directory, so a round-tripped pack is a first-class pack.
// ===================================================================
inline std::vector<uint8_t> serialize(PackHandleC h) {
    std::vector<uint8_t> out;
    const uint8_t* slab = PackTableC::instance().slab(h);
    if (!slab) return out;
    PackHeaderC hd;
    std::memcpy(&hd, slab, sizeof hd);
    const auto* dir = reinterpret_cast<const DirEntryC*>(slab + hd.dir_offset);
    auto& bt = BufTable::instance();
    // Size the wire in one pass so the append loop never regrows the vector
    // (a regrow re-copies megabytes and, in the first cut, made "slab memcpy
    // + append" slower than the walk it was supposed to beat).
    uint64_t total = hd.slab_bytes;
    for (uint32_t i = 0; i < hd.entry_count; ++i)
        if (dir[i].storage == kStorageExtern)
            total += 2 + 12 + 8 + bt.bytes(dir[i].u.ext);
    out.reserve(size_t(total));
    out.insert(out.end(), slab, slab + hd.slab_bytes);
    for (uint32_t i = 0; i < hd.entry_count; ++i) {
        if (dir[i].storage != kStorageExtern) continue;
        BufHandleC bh = dir[i].u.ext;
        uint16_t tid = bt.type_id(bh);
        uint32_t shp[3] = {bt.shape(bh, 0), bt.shape(bh, 1), bt.shape(bh, 2)};
        uint64_t nb = bt.bytes(bh);
        const uint8_t* p = bt.data(bh);
        // Append via insert, NOT resize+memcpy: resize value-initializes the
        // new tail (an extra full memset of megabytes before the copy lands).
        uint8_t rec[22];
        std::memcpy(rec, &tid, 2);
        std::memcpy(rec + 2, shp, 12);
        std::memcpy(rec + 14, &nb, 8);
        out.insert(out.end(), rec, rec + sizeof rec);
        if (p && nb) out.insert(out.end(), p, p + nb);
    }
    return out;
}

// kPackNull on malformed input. Prototype-level validation only: magic,
// version, and size bounds are checked; this is NOT the hardened ingress path.
inline PackHandleC deserialize(const uint8_t* data, size_t n) {
    if (!data || n < sizeof(PackHeaderC)) return kPackNull;
    PackHeaderC hd;
    std::memcpy(&hd, data, sizeof hd);
    if (hd.magic != kMagic || hd.version != kVersion) return kPackNull;
    if (hd.slab_bytes > n || hd.dir_offset != sizeof(PackHeaderC)) return kPackNull;
    if (hd.payload_offset < hd.dir_offset + hd.entry_count * sizeof(DirEntryC) ||
        hd.payload_offset > hd.slab_bytes)
        return kPackNull;

    uint8_t cls = 0;
    uint8_t* slab = static_cast<uint8_t*>(raw_class_alloc(hd.slab_bytes, &cls));
    if (!slab) return kPackNull;
    std::memcpy(slab, data, hd.slab_bytes);

    // Re-mint the EXTERN buffers, patching the fresh slab's directory. On any
    // truncation, roll back what was minted (no half-owned pack escapes).
    auto* dir = reinterpret_cast<DirEntryC*>(slab + hd.dir_offset);
    size_t at = hd.slab_bytes;
    auto& bt = BufTable::instance();
    std::vector<BufHandleC> minted;
    auto rollback = [&] {
        for (BufHandleC b : minted) bt.release(b);
        raw_class_free(slab, cls);
        return kPackNull;
    };
    for (uint32_t i = 0; i < hd.entry_count; ++i) {
        if (dir[i].storage != kStorageExtern) continue;
        if (at + 2 + 12 + 8 > n) return rollback();
        uint16_t tid; uint32_t shp[3]; uint64_t nb;
        std::memcpy(&tid, data + at, 2);  at += 2;
        std::memcpy(shp, data + at, 12);  at += 12;
        std::memcpy(&nb, data + at, 8);   at += 8;
        if (at + nb > n) return rollback();
        BufHandleC bh = bt.mint(tid, shp, nb, data + at);
        at += nb;
        if (bh == kBufNull) return rollback();
        minted.push_back(bh);
        dir[i].u.ext = bh;
    }
    return PackTableC::instance().mint(slab, cls);
}

// Convenience retain/release (one atomic each — the design-C claim).
inline void retain(PackHandleC h)  { PackTableC::instance().retain(h); }
inline void release(PackHandleC h) { PackTableC::instance().release(h); }

} // namespace packc
} // namespace xi
