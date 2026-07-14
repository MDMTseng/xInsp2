#pragma once
//
// xi_image_pool.hpp — host-side refcounted image pool (mostly lock-free).
//
// Each handle resolves to a fixed-position slot in a flat array; the
// slot holds an atomic pointer to a PoolEntry. Lookup, addref, release,
// and most queries are lock-free — no mutex is held across pixel
// access, so a thread killed by `TerminateThread` mid-call never
// orphans a lock that future ops would block on.
//
// Handle layout:
//   bits 0-15   slot index   (65 536 slots)
//   bits 16-55  generation   (1 trillion reuses per slot)
//   bits 56-63  reserved 0   (formerly the SHM-region discriminator tag;
//                              kept zero in all heap-pool handles for
//                              binary-ABI compatibility with xi_abi.h)
//
// Generation defends against ABA: a slot reused after release is
// stamped with the next generation, so a stale handle (held by a
// careless plugin past release) fails the lookup cleanly instead of
// pointing into the new occupant. The 40-bit space gives any single
// slot 1.1e12 generations — practical immortality.
//
// Caller contract: when accessing data() / width() / etc on a handle,
// the caller MUST hold a refcount on it. Otherwise another thread's
// release() can free the underlying PoolEntry mid-deref. (This was
// the contract under the old shared_mutex implementation too — the
// lock only protected the unordered_map structure, not the pointer
// the lookup returned.)
//

#include "xi_abi.h"
#include "xi_image.hpp"
#include "xi_instance_folders.hpp"
#include "xi_status_sink.hpp"
#include "xi_binary_sink.hpp"   // ABI v8: backs host_api.emit_binary (plugin -> WS push)
#include "xi_log_sink.hpp"      // P1-4/P1-3: backs host_api.log (plugin/script -> WS log)
#include "xi_compress_sink.hpp" // ABI v9: backs host_api.compress_image (host JPEG cache)
// [ABI v12 — xi_doc_pool.hpp / xi_doc_registry.hpp includes removed at THE CUT;
//  the Record yyjson-doc allocator + refcount host slots are gone.]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <malloc.h>     // _aligned_malloc / _aligned_free
#else
#include <cstdlib>      // posix_memalign
#endif

namespace xi {

// =====================================================================
// pixpool — size-class pixel-buffer recycling (perf/imagepool-sizeclass).
//
// Backported from the design-C prototype (tests/proto/xi_pack_c.hpp raw_class_alloc):
// the production create()/release() cycle used to pay `new PoolEntry` +
// `vector::resize` (heap alloc + zero-fill + first-touch page faults) per
// create and a full heap free per release — ~508 us for a 1920x1200 frame.
// This layer changes ONLY where the pixel bytes come from and where they go
// on free; every other ImagePool semantic (handles, generations, refcounts,
// owner sweep, WalkGuard) is untouched.
//
//   * 2^n size classes, 4 KiB .. 64 MiB, every buffer 64-byte aligned
//     (_aligned_malloc). A request over 64 MiB (ImagePool allows up to its
//     documented 1 GiB per-buffer cap) takes the DIRECT lane: exact-size
//     aligned heap alloc/free, never cached (kClassDirect).
//   * Free path: per-thread LIFO magazine (hottest-in-cache) -> mutexed
//     global overflow shelf -> _aligned_free. Alloc path: magazine ->
//     shelf -> fresh _aligned_malloc.
//   * BYTE BUDGETS (production requirement the prototype deferred) — all
//     constexpr-tunable below:
//       - magazine: at most kMagazineCap (4) buffers per class per thread,
//         additionally capped so one class holds at most kMagazineByteBudget
//         (64 MiB) per thread => the 64 MiB class keeps only 1 per thread.
//       - shelf: per class, at most kShelfMaxCount (32) buffers AND at most
//         kShelfByteBudget (128 MiB) => the 64 MiB class can never hoard
//         more than 2 buffers; the 4 KiB..4 MiB classes are count-capped.
//     An over-budget free goes straight to _aligned_free (evicted_frees).
//   * Teardown ordering: the shelf is INTENTIONALLY LEAKED (same doctrine as
//     ImagePool::instance() — process-lifetime, reclaimed by the OS), so a
//     per-thread magazine destructor that runs at ANY point during thread /
//     static teardown can always drain its survivors to the shelf — there is
//     no destroyed-shelf window. The magazine itself lives behind a
//     trivially-destructible thread_local POINTER slot: after the owning
//     thread_local's destructor runs (nulling the slot), a late pixel_free
//     on that thread (e.g. from another thread_local's dtor releasing a
//     handle) safely bypasses the dead magazine and uses the shelf / heap.
// =====================================================================
namespace pixpool {

inline constexpr int      kMinClassLog        = 12;              // 4 KiB
inline constexpr int      kMaxClassLog        = 26;              // 64 MiB
inline constexpr int      kNumClasses         = kMaxClassLog - kMinClassLog + 1;
inline constexpr uint8_t  kClassDirect        = 0xFF;            // > 64 MiB lane
// Budgets (see the header block above for the rationale + resulting counts).
inline constexpr int      kMagazineCap        = 4;               // bufs/class/thread
inline constexpr uint64_t kMagazineByteBudget = 64ull << 20;     // per class per thread
inline constexpr int      kShelfMaxCount      = 32;              // bufs/class global
inline constexpr uint64_t kShelfByteBudget    = 128ull << 20;    // per class global

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

inline uint64_t class_bytes(uint8_t cls) {
    return uint64_t(1) << (kMinClassLog + cls);
}
inline uint8_t class_for(uint64_t bytes) {
    if (bytes == 0) bytes = 1;
    for (int c = kMinClassLog; c <= kMaxClassLog; ++c)
        if (bytes <= (uint64_t(1) << c)) return uint8_t(c - kMinClassLog);
    return kClassDirect;
}
// Effective per-class caps under the byte budgets.
inline int magazine_slots(uint8_t cls) {
    uint64_t by_budget = kMagazineByteBudget / class_bytes(cls);
    int n = by_budget < uint64_t(kMagazineCap) ? int(by_budget) : kMagazineCap;
    return n < 1 ? 1 : n;    // every class may keep at least one
}
inline int shelf_slots(uint8_t cls) {
    uint64_t by_budget = kShelfByteBudget / class_bytes(cls);
    return by_budget < uint64_t(kShelfMaxCount) ? int(by_budget) : kShelfMaxCount;
}

// Cumulative counters (relaxed; diagnostic only). The tests' oracle for
// "the magazine actually recycled" — mirrored by ImagePool::pixel_alloc_stats().
struct Stats {
    std::atomic<uint64_t> magazine_hits{0};   // alloc served from the magazine
    std::atomic<uint64_t> shelf_hits{0};      // alloc served from the shelf
    std::atomic<uint64_t> fresh_allocs{0};    // alloc fell through to the heap
    std::atomic<uint64_t> direct_allocs{0};   // > 64 MiB direct-lane alloc
    std::atomic<uint64_t> magazine_puts{0};   // free cached in the magazine
    std::atomic<uint64_t> shelf_puts{0};      // free cached on the shelf
    std::atomic<uint64_t> evicted_frees{0};   // free over budget -> heap
    std::atomic<uint64_t> direct_frees{0};    // direct-lane free
};
inline Stats& stats() {
    static Stats s;
    return s;
}

// One pooled pixel buffer. cap is the usable capacity (class size, or the
// exact request on the direct lane); the logical image size lives on the
// PoolEntry.
struct PixBuf {
    uint8_t* mem = nullptr;
    uint64_t cap = 0;
    uint8_t  cls = 0;
};

namespace detail {

// Global overflow shelf — cold path (magazine over/underflow, thread exit).
// INTENTIONALLY LEAKED, same doctrine as ImagePool::instance(): magazine
// destructors may run arbitrarily late in thread/static teardown and must
// always find a live shelf.
class Shelf {
public:
    static Shelf& instance() {
        static Shelf* s = new Shelf();
        return *s;
    }
    void* pop(uint8_t cls) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& v = free_[cls];
        if (v.empty()) return nullptr;
        void* p = v.back();
        v.pop_back();
        return p;
    }
    // False when the class is at its budget (caller frees outright).
    bool push(uint8_t cls, void* p) {
        std::lock_guard<std::mutex> lk(mu_);
        auto& v = free_[cls];
        if (int(v.size()) >= shelf_slots(cls)) return false;
        v.push_back(p);
        return true;
    }
    // Diagnostic (tests): current cached count for a class.
    int count(uint8_t cls) {
        std::lock_guard<std::mutex> lk(mu_);
        return int(free_[cls].size());
    }

private:
    std::mutex mu_;
    std::vector<void*> free_[kNumClasses];
};

// Per-thread magazines. The destructor drains survivors to the (leaked,
// hence always-alive) shelf, or to the heap when the shelf is at budget —
// a dying thread never leaks its cached buffers.
struct Magazine {
    void* items[kNumClasses][kMagazineCap];
    int   count[kNumClasses];
    Magazine() { std::memset(count, 0, sizeof count); }
    ~Magazine() {
        for (int c = 0; c < kNumClasses; ++c)
            for (int i = 0; i < count[c]; ++i)
                if (!Shelf::instance().push(uint8_t(c), items[c][i]))
                    aligned_free64(items[c][i]);
    }
    Magazine(const Magazine&) = delete;
    Magazine& operator=(const Magazine&) = delete;
};

// The magazine hides behind a trivially-destructible thread_local POINTER:
// the pointer slot itself has no destructor, so it stays readable (as null)
// even after the owning Owner thread_local has been destroyed — a release()
// running inside a LATER thread_local destructor on the same thread cannot
// touch a dead magazine; it falls through to the shelf / heap instead.
inline Magazine* tls_magazine() {
    thread_local Magazine* slot = nullptr;
    struct Owner {
        Magazine** s;
        explicit Owner(Magazine** slot_) : s(slot_) { *s = new Magazine(); }
        ~Owner() { delete *s; *s = nullptr; }
    };
    thread_local Owner owner{&slot};
    return slot;
}

} // namespace detail

// Alloc: magazine -> shelf -> fresh heap; > 64 MiB takes the direct lane.
// Returned bytes are UNSPECIFIED (recycled buffers carry stale pixels);
// ImagePool::create() decides the zero-fill policy.
inline PixBuf pixel_alloc(uint64_t bytes) {
    PixBuf b;
    b.cls = class_for(bytes);
    if (b.cls == kClassDirect) {
        b.mem = static_cast<uint8_t*>(aligned_alloc64(size_t(bytes)));
        b.cap = b.mem ? bytes : 0;
        if (b.mem) stats().direct_allocs.fetch_add(1, std::memory_order_relaxed);
        return b;
    }
    b.cap = class_bytes(b.cls);
    if (detail::Magazine* m = detail::tls_magazine(); m && m->count[b.cls] > 0) {
        b.mem = static_cast<uint8_t*>(m->items[b.cls][--m->count[b.cls]]);   // hot
        stats().magazine_hits.fetch_add(1, std::memory_order_relaxed);
        return b;
    }
    if (void* p = detail::Shelf::instance().pop(b.cls)) {
        b.mem = static_cast<uint8_t*>(p);
        stats().shelf_hits.fetch_add(1, std::memory_order_relaxed);
        return b;
    }
    b.mem = static_cast<uint8_t*>(aligned_alloc64(size_t(b.cap)));
    if (b.mem) stats().fresh_allocs.fetch_add(1, std::memory_order_relaxed);
    else       b.cap = 0;
    return b;
}

// Free: magazine (releasing thread's — a buffer created on thread A and
// released on thread B migrates to B's magazine) -> shelf -> heap.
inline void pixel_free(PixBuf& b) {
    if (!b.mem) return;
    if (b.cls == kClassDirect) {
        aligned_free64(b.mem);
        stats().direct_frees.fetch_add(1, std::memory_order_relaxed);
    } else if (detail::Magazine* m = detail::tls_magazine();
               m && m->count[b.cls] < magazine_slots(b.cls)) {
        m->items[b.cls][m->count[b.cls]++] = b.mem;
        stats().magazine_puts.fetch_add(1, std::memory_order_relaxed);
    } else if (detail::Shelf::instance().push(b.cls, b.mem)) {
        stats().shelf_puts.fetch_add(1, std::memory_order_relaxed);
    } else {
        aligned_free64(b.mem);
        stats().evicted_frees.fetch_add(1, std::memory_order_relaxed);
    }
    b.mem = nullptr;
    b.cap = 0;
}

} // namespace pixpool

// Per-creator identity. Lets the pool sweep all handles allocated on
// behalf of a given plugin instance / script when that owner dies
// (instance destroyed, worker process exited, script unloaded). An
// owner of 0 means "anonymous / framework" — handles created with no
// owner context (e.g. the backend's own grab path) are never swept.
using ImagePoolOwnerId = uint32_t;

struct PoolEntry {
    // Pooled pixel storage (perf/imagepool-sizeclass): a 64-byte-aligned
    // buffer from the pixpool size-class recycler; `size` is the logical
    // byte count (w*h*ch, <= buf.cap). The destructor returns the buffer to
    // the recycler, so EVERY existing delete path (reclaim_entry_,
    // drain_retired_, the failed-create unique_ptr) recycles for free.
    pixpool::PixBuf buf;
    uint64_t size = 0;
    int32_t  width = 0;
    int32_t  height = 0;
    int32_t  channels = 0;
    std::atomic<int32_t> refcount{1};
    uint64_t generation = 0;        // matches handle's generation field
    // owner: who allocated this; 0 = anonymous. ATOMIC because it is written by
    // release_all_for() (owner sweep, sets 0 on a spared entry) on one thread
    // while the diagnostic stats walk reads it on another — a plain field would
    // be a formal data race (external review 08 finding 1). Relaxed everywhere:
    // stats only needs a coherent-enough snapshot, not ordering against pixels.
    std::atomic<ImagePoolOwnerId> owner{0};

    ~PoolEntry() { pixpool::pixel_free(buf); }
};

class ImagePool {
public:
    static constexpr uint32_t SLOT_BITS  = 16;
    static constexpr uint32_t SLOT_COUNT = 1u << SLOT_BITS;   // 65 536
    static constexpr uint64_t SLOT_MASK  = SLOT_COUNT - 1;
    // Max generation that fits in (64 - 8 - SLOT_BITS) = 40 bits.
    static constexpr uint64_t GEN_MAX    = (1ull << 40) - 1;
    // create() returns ZEROED pixels — the documented contract since the
    // vector-value-init days; recycled buffers are memset to preserve it
    // (see create()). Keep true unless every read-before-write caller is gone.
    static constexpr bool kZeroFillCreate = true;

    // INTENTIONALLY LEAKED: the pool is process-lifetime state, so it is never
    // destroyed — heap-allocated once, reclaimed by the OS at exit. This makes
    // every late-teardown path (a static-destruction adapter dtor's owner
    // sweep, a deferred LoadedScript module_lifetime deleter) safe to call the
    // pool unconditionally: there is no destroyed-Meyers-singleton window, and
    // no pool-liveness guard flag to keep in sync.
    static ImagePool& instance() {
        static ImagePool* pool = new ImagePool();
        return *pool;
    }

    // ---- core lookup -------------------------------------------------

    PoolEntry* lookup(xi_image_handle h) const { return lookup_(h, nullptr); }

    // ---- create / release -------------------------------------------

    // create() returns ZEROED pixels (the documented external contract).
    // create_uninit() is the identical mint but SKIPS the zero-fill — for a
    // caller that immediately full-overwrites the buffer (pack_pool::alloc_bytes
    // memcpy's a non-null src over every byte), where the memset is pure waste
    // (doc 28 zeroinit verdict). NEVER use create_uninit for a writable canvas a
    // consumer may read before writing — that path keeps create()'s zero-fill.
    xi_image_handle create(int32_t w, int32_t h, int32_t ch) {
        return create_(w, h, ch, /*zero_fill=*/true);
    }
    xi_image_handle create_uninit(int32_t w, int32_t h, int32_t ch) {
        return create_(w, h, ch, /*zero_fill=*/false);
    }

private:
    xi_image_handle create_(int32_t w, int32_t h, int32_t ch, bool zero_fill) {
        // D-P1-7: validate dimensions BEFORE entering counter / slot
        // bookkeeping. The original `(size_t)w * h * ch` cast applies
        // only to the first multiplicand; `h * ch` is int32 mul first,
        // signed-overflow UB for big inputs. And the previous code
        // incremented live_count_ / total_created_ before the slot
        // acquire — when the pool was exhausted those counters drifted
        // permanently. Reject + early-out before any side effect.
        if (w <= 0 || h <= 0 || ch <= 0) return 0;
        const int64_t pixels =
            int64_t(w) * int64_t(h) * int64_t(ch);
        // Cap at 1 GiB per image to avoid runaway allocations from a
        // hostile / careless plugin; INT32_MAX is technically the wider
        // limit but 1 GiB is comfortably above any real CV input and
        // keeps each create's allocator pressure bounded.
        if (pixels <= 0 || pixels > (int64_t(1) << 30)) return 0;

        std::unique_ptr<PoolEntry> entry(new PoolEntry());
        entry->buf = pixpool::pixel_alloc((uint64_t)pixels);
        if (!entry->buf.mem) {
            // entry deletes via unique_ptr; counters untouched.
            return 0;
        }
        entry->size = (uint64_t)pixels;
        // ZERO-FILL: create() has always returned zeroed pixels (the old
        // vector::resize value-init), and callers exist that rely on it
        // (e.g. plugins that paint shapes onto a "blank" canvas). A recycled
        // magazine buffer carries the PREVIOUS image's bytes, so we memset
        // the logical size explicitly. Still ~5-10x cheaper than the old
        // path for the hot same-size case: the memset writes warm, already-
        // faulted-in pages instead of malloc + zero + first-touch faults.
        // Flip kZeroFillCreate ONLY with proof no caller reads-before-write.
        // zero_fill=false (create_uninit) skips it for a caller that immediately
        // full-overwrites the buffer.
        if (kZeroFillCreate && zero_fill)
            std::memset(entry->buf.mem, 0, (size_t)pixels);
        entry->width    = w;
        entry->height   = h;
        entry->channels = ch;
        entry->refcount.store(1, std::memory_order_relaxed);
        entry->owner.store(current_owner(), std::memory_order_relaxed);

        uint32_t idx = acquire_slot_();
        if (idx == 0xFFFFFFFFu) {       // pool exhausted
            std::fprintf(stderr,
                "[xinsp2] ImagePool exhausted (cap=%u live handles)\n",
                SLOT_COUNT);
            return 0;
        }
        // Past the failure points; commit counters now (cumulative
        // never decrements, live_count_ tracks actual occupancy).
        uint64_t cum = total_created_.fetch_add(1, std::memory_order_relaxed) + 1;
        (void)cum;
        int32_t live = live_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        int32_t hw = high_water_.load(std::memory_order_relaxed);
        while (live > hw &&
               !high_water_.compare_exchange_weak(hw, live,
                       std::memory_order_relaxed)) {}
        // Hand ownership to the slot.
        PoolEntry* raw = entry.release();
        // Bump the slot's running generation; stamp it into the entry.
        uint64_t gen = (slots_[idx].generation.fetch_add(1, std::memory_order_relaxed) + 1)
                       & GEN_MAX;
        raw->generation = gen;
        slots_[idx].entry.store(raw, std::memory_order_release);

        return ((uint64_t)gen << SLOT_BITS) | (uint64_t)idx;
    }

public:
    void addref(xi_image_handle h) {
        if (auto* e = lookup(h)) {
            e->refcount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void release(xi_image_handle h) {
        uint32_t idx = 0;
        PoolEntry* e = lookup_(h, &idx);
        if (!e) return;
        if (e->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Last ref — clear slot, return to free list, reclaim entry.
            // The slot-null store is seq_cst (not merely release) so it is
            // globally ordered against the active_walkers_ load in
            // reclaim_entry_(): together they form the StoreLoad handshake that
            // lets a diagnostic stats walk never dereference a freed entry
            // (see reclaim_entry_).
            slots_[idx].entry.store(nullptr, std::memory_order_seq_cst);
            release_slot_(idx);
            reclaim_entry_(e);
            live_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // ---- queries (lock-free) -----------------------------------------

    uint8_t* data(xi_image_handle h) {
        auto* e = lookup(h);
        return e ? e->buf.mem : nullptr;
    }

    // Read-only pixel pointer — the blessed accessor for ANY handle (input or
    // output). Same bytes data() returns, const-qualified so it can never be a
    // mutation path. Backs xi.imaging_rw@1.image_read. Null on a bad handle.
    const uint8_t* read_data(xi_image_handle h) {
        auto* e = lookup(h);
        return e ? e->buf.mem : nullptr;
    }

    // Writable pixel pointer VALID ONLY for a uniquely-owned handle (refcount ==
    // 1 — a freshly-created output that no other consumer aliases). Returns NULL
    // for a SHARED handle (an input aliased across consumers, refcount > 1): the
    // strict, correct behaviour (external review 02 I.4). NO silent copy-on-write
    // — a plugin that wants to mutate a shared input must allocate its own output
    // via create() and write there. Backs xi.imaging_rw@1.image_write.
    //
    // The refcount is read with acquire ordering; a caller holding its own ref
    // (the contract for touching any handle) means a concurrent release can only
    // take the count from N to N-1, never resurrect a freed slot — so a count of
    // 1 observed here genuinely means "this caller is the sole holder".
    uint8_t* writable_data(xi_image_handle h) {
        auto* e = lookup(h);
        if (!e) return nullptr;
        if (e->refcount.load(std::memory_order_acquire) != 1) return nullptr;
        return e->buf.mem;
    }
    int32_t width(xi_image_handle h) {
        auto* e = lookup(h);  return e ? e->width  : 0;
    }
    int32_t height(xi_image_handle h) {
        auto* e = lookup(h);  return e ? e->height : 0;
    }
    int32_t channels(xi_image_handle h) {
        auto* e = lookup(h);  return e ? e->channels : 0;
    }
    int32_t stride(xi_image_handle h) {
        auto* e = lookup(h);  return e ? e->width * e->channels : 0;
    }

    xi_image_handle from_image(const Image& img) {
        if (img.empty()) return XI_IMAGE_NULL;
        auto h = create(img.width, img.height, img.channels);
        if (h) std::memcpy(data(h), img.data(), img.size());
        return h;
    }

    Image to_image(xi_image_handle h) {
        auto* e = lookup(h);
        if (!e) return {};
        return Image(e->width, e->height, e->channels, e->buf.mem);
    }

    // ---- Owner ledger ------------------------------------------------

    static ImagePoolOwnerId alloc_owner_id() {
        static std::atomic<ImagePoolOwnerId> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
    static ImagePoolOwnerId& current_owner_ref() {
        static thread_local ImagePoolOwnerId v = 0;
        return v;
    }
    static ImagePoolOwnerId  current_owner() { return current_owner_ref(); }

    struct OwnerGuard {
        ImagePoolOwnerId prev;
        explicit OwnerGuard(ImagePoolOwnerId next) : prev(current_owner_ref()) {
            current_owner_ref() = next;
        }
        ~OwnerGuard() { current_owner_ref() = prev; }
        OwnerGuard(const OwnerGuard&) = delete;
        OwnerGuard& operator=(const OwnerGuard&) = delete;
    };

    // Drop THIS owner's claim on every entry it allocated, then reclaim only
    // the entries no one else still holds.
    //
    // Owner-sweep contract (cold lifecycle path — instance destroy / hot-
    // recompile / rename; NOT the per-frame hot path):
    //
    // This used to force-`delete` every owner==P entry REGARDLESS of refcount,
    // on the comment's assumption that "owner is gone ⇒ no remaining consumer is
    // legitimate". That assumption is FALSE under the pool's own zero-copy
    // cross-instance sharing: when a producer P forwards its pool handle to a
    // downstream consumer Q, record_to_c does `image_addref` on P's handle and
    // Q adopts it via record_from_c → adopt_pool_handle (another addref + a
    // cached raw pixel pointer inside Q's xi::Image). A caching consumer
    // (buffer_replay / gathering / accumulator) legitimately keeps that ref
    // across calls on a PoolEntry still tagged owner==P. Force-freeing it when
    // P's adapter was destroyed left Q with a dangling xi::Image → UAF on the
    // next replay, or a failed generation check → a silently dropped frame.
    //
    // New contract: releasing an owner drops EXACTLY ONE ref per entry — P's own
    // ownership ref — precisely as if P had called release() on each handle it
    // allocated:
    //   * refcount==1 (P is the sole holder — a genuine leak, no external
    //     consumer): hits zero ⇒ reclaimed immediately. The original leak-sweep
    //     intent is preserved.
    //   * refcount>1 (a live external consumer Q still holds it): survives with
    //     its owner neutralised to anonymous (0) so it is neither re-swept nor
    //     tied to P's lifetime; the LAST holder frees it via the normal
    //     release() path. We do NOT clear the slot or bump its generation for a
    //     spared entry, so Q's outstanding handle still resolves through
    //     lookup().
    //
    // Returns the count of entries actually RECLAIMED (i.e. genuine leaks);
    // spared still-referenced entries are not counted (they were never leaks).
    int release_all_for(ImagePoolOwnerId owner) {
        if (owner == 0) return 0;
        // This is itself a full-pool slot walk that dereferences entries; a
        // concurrent stats() walk (or another release_all_for) may be reading
        // the same entries. Announce ourselves as a walker so any concurrent
        // release() defers its frees, and route our own frees through
        // reclaim_entry_ so a concurrent walker never sees a freed entry.
        WalkGuard wg(*this);
        int swept = 0;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            // seq_cst (not acquire): this load must sit in the same total order
            // as the releaser's null-store (:327) and the WalkGuard increment,
            // so the StoreLoad handshake formally closes on weak-memory targets
            // (see the deferred-reclamation note above).
            PoolEntry* e = slots_[i].entry.load(std::memory_order_seq_cst);
            if (!e || e->owner.load(std::memory_order_relaxed) != owner) continue;
            // Drop P's ownership ref — same accounting as release().
            if (e->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // Sole holder (leak): clear slot, return to free list, reclaim.
                // seq_cst store pairs with reclaim_entry_'s walker check.
                slots_[i].entry.store(nullptr, std::memory_order_seq_cst);
                release_slot_(i);
                reclaim_entry_(e);
                ++swept;
            } else {
                // A live external consumer still references this entry. Orphan
                // it (anonymous owner) so it outlives P and is freed by its last
                // holder; leave the slot + generation intact so that handle
                // still resolves.
                e->owner.store(0, std::memory_order_relaxed);
            }
        }
        if (swept > 0) live_count_.fetch_sub(swept, std::memory_order_relaxed);
        return swept;
    }

    struct GlobalCum {
        uint64_t total_created = 0;   // lifetime allocation count
        int32_t  high_water    = 0;   // max simultaneous live count seen
        int32_t  live_now      = 0;   // current live (== stats().handle_count)
    };
    GlobalCum cumulative() const {
        GlobalCum g;
        g.total_created = total_created_.load(std::memory_order_relaxed);
        g.high_water    = high_water_.load(std::memory_order_relaxed);
        g.live_now      = live_count_.load(std::memory_order_relaxed);
        return g;
    }

    // Snapshot of the pixel-buffer recycler's counters (pixpool). Diagnostic
    // only (relaxed); the recycle tests' oracle for magazine/shelf behaviour.
    struct PixAllocStats {
        uint64_t magazine_hits = 0, shelf_hits = 0, fresh_allocs = 0,
                 direct_allocs = 0;
        uint64_t magazine_puts = 0, shelf_puts = 0, evicted_frees = 0,
                 direct_frees = 0;
    };
    static PixAllocStats pixel_alloc_stats() {
        auto& s = pixpool::stats();
        PixAllocStats out;
        out.magazine_hits = s.magazine_hits.load(std::memory_order_relaxed);
        out.shelf_hits    = s.shelf_hits.load(std::memory_order_relaxed);
        out.fresh_allocs  = s.fresh_allocs.load(std::memory_order_relaxed);
        out.direct_allocs = s.direct_allocs.load(std::memory_order_relaxed);
        out.magazine_puts = s.magazine_puts.load(std::memory_order_relaxed);
        out.shelf_puts    = s.shelf_puts.load(std::memory_order_relaxed);
        out.evicted_frees = s.evicted_frees.load(std::memory_order_relaxed);
        out.direct_frees  = s.direct_frees.load(std::memory_order_relaxed);
        return out;
    }

    struct OwnerStats {
        int      handle_count = 0;
        uint64_t total_bytes  = 0;
    };
    OwnerStats stats(ImagePoolOwnerId owner = 0) {
        // WalkGuard makes this read-only slot walk memory-safe against a
        // concurrent release(): while any walker is live, a last-ref release
        // defers its `delete` to the retire list instead of freeing under our
        // pointer, so `e->size` here can never touch a freed entry
        // (external review 08 finding 1). Zero cost when no walk is in flight.
        WalkGuard wg(*this);
        OwnerStats s{};
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            // seq_cst: keeps this walk's slot read in the StoreLoad total order
            // with the releaser's null-store, closing the handshake on weak
            // memory (see the deferred-reclamation note above).
            PoolEntry* e = slots_[i].entry.load(std::memory_order_seq_cst);
            if (!e) continue;
            if (owner != 0 && e->owner.load(std::memory_order_relaxed) != owner) continue;
            ++s.handle_count;
            s.total_bytes += e->size;
        }
        return s;
    }

    struct PerOwnerStat {
        ImagePoolOwnerId owner = 0;
        int              handle_count = 0;
        uint64_t         total_bytes  = 0;
    };
    std::vector<PerOwnerStat> stats_by_owner() {
        // Same deferred-reclamation guard as stats() — see there.
        WalkGuard wg(*this);
        std::unordered_map<ImagePoolOwnerId, PerOwnerStat> agg;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            // seq_cst: same StoreLoad-handshake reason as stats() above.
            PoolEntry* e = slots_[i].entry.load(std::memory_order_seq_cst);
            if (!e) continue;
            ImagePoolOwnerId ow = e->owner.load(std::memory_order_relaxed);
            auto& s = agg[ow];
            s.owner = ow;
            ++s.handle_count;
            s.total_bytes += e->size;
        }
        std::vector<PerOwnerStat> out;
        out.reserve(agg.size());
        for (auto& [_, s] : agg) out.push_back(s);
        return out;
    }

    // ---- host_api factory --------------------------------------------

    static xi_host_api make_host_api() {
        xi_host_api api = {};
        api.image_create   = [](int32_t w, int32_t h, int32_t ch) -> xi_image_handle {
            return ImagePool::instance().create(w, h, ch);
        };
        api.image_addref   = [](xi_image_handle h) { ImagePool::instance().addref(h); };
        api.image_release  = [](xi_image_handle h) { ImagePool::instance().release(h); };
        api.image_data     = [](xi_image_handle h) -> uint8_t* {
            return ImagePool::instance().data(h);
        };
        api.image_width    = [](xi_image_handle h) -> int32_t {
            return ImagePool::instance().width(h);
        };
        api.image_height   = [](xi_image_handle h) -> int32_t {
            return ImagePool::instance().height(h);
        };
        api.image_channels = [](xi_image_handle h) -> int32_t {
            return ImagePool::instance().channels(h);
        };
        api.image_stride   = [](xi_image_handle h) -> int32_t {
            return ImagePool::instance().stride(h);
        };
        api.log            = [](int32_t level, const char* msg) {
            const char* lvl[] = {"DEBUG", "INFO", "WARN", "ERROR"};
            // Wall-clock timestamp so a backend log line is correlatable with a
            // client-visible symptom / a crash sidecar (which stamps ts_ms) by time
            // — a bare "[ERROR] msg" with no time is near-useless in production.
            auto now = std::chrono::system_clock::now();
            auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now.time_since_epoch()).count() % 1000;
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            std::tm tmv{};
#ifdef _WIN32
            localtime_s(&tmv, &t);
#else
            localtime_r(&t, &tmv);   // TODO(linux): localtime_r arg order is (time_t*, tm*)
#endif
            char ts[16];
            std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
                          tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (int)ms);
            std::fprintf(stderr, "[%s %s] %s\n", ts, lvl[level & 3], msg);
            // P1-3: also forward to the operator channel (WS log / recent-errors)
            // via the installed sink, so a plugin's WARN/ERROR isn't lost on an
            // unwatched stderr. No-op when no sink is installed (headless). The
            // sink itself decides which levels escalate; we pass the full epoch ms.
            if (auto fn = xi::log_sink()) {
                int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now.time_since_epoch()).count();
                fn(level, msg ? msg : "", ts_ms);
            }
        };
        api.instance_folder = [](const char* name, char* buf, int32_t buflen) -> int32_t {
            std::string p = InstanceFolderRegistry::instance().get(name ? name : "");
            int32_t n = (int32_t)p.size();
            if (n == 0) return 0;
            if (buflen < n + 1) return -n;
            std::memcpy(buf, p.data(), n);
            buf[n] = 0;
            return n;
        };
        // [ABI v12 — emit_record + read_image_file host slots were DELETED at THE
        // CUT. Sources emit sealed packs (xi_pack_v1::emit_pack, wired below);
        // image decode is the xi.image.decode capability. decode_image_fn()
        // still exists as an INTERNAL host helper (cmd:run disk frame injection,
        // service_cmd_dispatch.cpp), now capability-only — it is just no longer
        // published as a plugin-facing host_api slot.]
        // Routes to the backend's status registry via the installed sink
        // (xi_status_sink.hpp); no-op when no sink is installed (headless).
        api.set_status = [](const char* source, const char* text) {
            if (auto fn = xi::status_sink()) fn(source, text);
        };
        // ABI v8: push an opaque binary frame to WS clients via the installed
        // sink (xi_binary_sink.hpp); no-op when no sink is installed (headless).
        api.emit_binary = [](const void* data, int32_t len) {
            if (auto fn = xi::binary_sink()) fn(data, len);
        };
        // ABI v9: JPEG-encode through the host cache via the installed sink
        // (xi_compress_sink.hpp); returns 0 when no encoder is installed.
        // Routed through compress_image_impl so the legacy field and the carved
        // xi.preview@1 interface (get_interface, below) share the IDENTICAL path.
        api.compress_image = &compress_image_impl;
        // [ABI v12 — the host doc allocator (doc_chunk_*) and doc refcount
        // (doc_retain/release/refcount) slots were DELETED at THE CUT with the
        // Record yyjson-doc dispatch path. Packs carry typed entries directly;
        // there is no cross-ABI yyjson doc. DocChunkPool / DocRegistry are gone.]
        // ABI v10: the capability-query door. A plugin resolves a frozen,
        // segregated interface by id+version through this one pointer
        // (core_fix_plan.md §12 Phase 1). Registrations live in
        // get_interface_impl: the carved xi.preview/imaging/log@1 + xi.pack@1 +
        // xi.cap interfaces (xi.doc/xi.emit-emit_record retired at THE CUT).
        api.get_interface = &get_interface_impl;
        return api;
    }

    // ---- capability-query door (ABI v10, core_fix_plan.md §11-12) -----

    // The JPEG-encode-with-host-cache capability, shared VERBATIM by the legacy
    // compress_image field and the carved xi.preview@1 interface, so both reach
    // the identical host code path (the installed content-addressed compress
    // sink). Returns 0 when no encoder is installed.
    static int32_t compress_image_impl(const void* px, int32_t w, int32_t h,
                                       int32_t c, int32_t q,
                                       void* out, int32_t cap) {
        if (auto fn = xi::compress_sink()) return fn(px, w, h, c, q, out, cap);
        return 0;
    }

    // xi.preview@1 — the first segregated interface carved out of the v9
    // monolith (Phase 2). A single-entry, frozen struct wrapping the SAME
    // compress_image_impl as the legacy field, so the door and the field
    // compress byte-for-byte identically. Process-stable address (Meyers
    // singleton); the host hands plugins a borrowed pointer they cache once.
    static const xi_preview_v1* preview_v1_iface() {
        static const xi_preview_v1 iface = { &compress_image_impl };
        return &iface;
    }

    // xi.imaging@1 / xi.doc@1 / xi.emit@1 / xi.log@1 — the remaining capability
    // domains carved out of the v9 monolith (Phase 3). Each is a frozen struct
    // whose entries are COPIED from the canonical host table, so every interface
    // fn-pointer is byte-for-byte the SAME pointer as the legacy xi_host_api
    // field (the door and the field hit one code path; an old plugin keeps using
    // the field forever). Process-stable address (Meyers singleton), built lazily
    // from canonical_host_api() — no static-init ordering hazard, no recursion
    // (building the table only STORES &get_interface_impl, never calls it).
    // xi.imaging_rw@1 — the read/write access-discipline interface (external
    // review 02 I.4). image_read const-qualifies the pool bytes; image_write
    // gates on unique ownership (refcount == 1) and returns null for a shared
    // input. Stable free-function trampolines so the interface entries are
    // process-stable addresses a plugin may cache. Both reach the SAME pool bytes
    // as image_data.
    static const uint8_t* image_read_impl(xi_image_handle h) {
        return ImagePool::instance().read_data(h);
    }
    static uint8_t* image_write_impl(xi_image_handle h) {
        return ImagePool::instance().writable_data(h);
    }
    static const xi_imaging_rw_v1* imaging_rw_v1_iface() {
        static const xi_imaging_rw_v1 iface = { &image_read_impl, &image_write_impl };
        return &iface;
    }

    static const xi_imaging_v1* imaging_v1_iface() {
        static const xi_imaging_v1 iface = [] {
            const xi_host_api* h = canonical_host_api();
            xi_imaging_v1 i{};
            i.image_create    = h->image_create;
            i.image_addref    = h->image_addref;
            i.image_release   = h->image_release;
            i.image_data      = h->image_data;
            i.image_width     = h->image_width;
            i.image_height    = h->image_height;
            i.image_channels  = h->image_channels;
            i.image_stride    = h->image_stride;
            // [ABI v12 — read_image_file dropped from xi.imaging@1 at THE CUT.]
            return i;
        }();
        return &iface;
    }
    // [ABI v12 — doc_v1_iface() (xi.doc@1) and the xi.emit@1 emit_record wiring
    //  bridge (EmitRecordFn / emit_record_slot / publish_emit_record / the
    //  forwarder) were DELETED at THE CUT. The Record yyjson-doc + Record
    //  source-emit path is gone; sources emit sealed packs (xi.pack@1 emit_pack).]

    // polaris2 wave-2: the carved xi.pack@1 DATA-PLANE interface (const
    // xi_pack_v1*) is published here by xi::install_pack_abi (xi_pack_abi.hpp).
    // Same layering bridge as emit_record: image_pool.hpp CANNOT include
    // xi_pack_abi.hpp (which sits ABOVE it — xi_pack_abi includes xi_pack.hpp,
    // which includes THIS header), so the door serves a slot-published pointer
    // instead of building the interface from canonical_host_api(). Null until the
    // pack ABI is installed (a host with no pack plane returns NULL for the id).
    static std::atomic<const void*>& pack_iface_slot() {
        static std::atomic<const void*> slot{nullptr};
        return slot;
    }
    static void publish_pack_iface(const void* iface) {
        pack_iface_slot().store(iface, std::memory_order_release);
    }
    // pack-v3: the xi.pack@3 slab-generation supplement (const xi_pack_v3* —
    // tensor entries, user-typed blobs, adopt_bin, ordinal type_id/entry
    // iteration), published by the same install_pack_abi through the same
    // layering bridge. Null until installed / on a host predating @3.
    static std::atomic<const void*>& pack3_iface_slot() {
        static std::atomic<const void*> slot{nullptr};
        return slot;
    }
    static void publish_pack3_iface(const void* iface) {
        pack3_iface_slot().store(iface, std::memory_order_release);
    }

    // Pack-plane hardening: the PackRegistry OWNER-SWEEP bridge — the pack
    // analogue of release_all_for's leak sweep. Published by install_pack_abi
    // (same layering bridge as pack_iface_slot: this header cannot include
    // xi_pack_abi.hpp). The teardown paths that sweep leaked image handles
    // (CAbiInstanceAdapter dtor, script unload) call sweep_packs_for so a
    // pack-retaining plugin that forgets to release on destroy is counted and
    // reclaimed instead of silently leaking sealed packs in the registry.
    // Returns the number of leaked pack refs dropped; 0 until published.
    using PackSweepFn = int (*)(ImagePoolOwnerId owner);
    static std::atomic<PackSweepFn>& pack_sweep_slot() {
        static std::atomic<PackSweepFn> slot{nullptr};
        return slot;
    }
    static void publish_pack_sweeper(PackSweepFn fn) {
        pack_sweep_slot().store(fn, std::memory_order_release);
    }
    static int sweep_packs_for(ImagePoolOwnerId owner) {
        if (auto fn = pack_sweep_slot().load(std::memory_order_acquire))
            return fn(owner);
        return 0;
    }

    // Pack ownership HANDOFF bridge (the phantom-ledger fix; f_emit_pack's
    // retain_untagged doctrine, xi_pack_abi.hpp). A producer-side funnel (the
    // capability funnel, run_pack_door) runs the plugin under OwnerGuard, so
    // the sealed OUTPUT pack's initial PackRegistry ref is owner-tagged to the
    // PRODUCER — but that ref is handed to the CALLER, who owns it. Left
    // tagged, the ledger carries a phantom {producer,1} charge and the
    // producer's teardown sweep (sweep_packs_for → release_all_for) would
    // reclaim it and free the caller's live pack (UAF / data loss). This
    // bridge moves that one ref's tag off the producer: +1 untagged then -1
    // tagged — net refcount unchanged. Same layering bridge as
    // pack_sweep_slot (this header cannot include xi_pack_abi.hpp); published
    // by install_pack_abi, no-op until then.
    using PackUntagFn = void (*)(xi_pack_handle f, ImagePoolOwnerId owner);
    static std::atomic<PackUntagFn>& pack_untag_slot() {
        static std::atomic<PackUntagFn> slot{nullptr};
        return slot;
    }
    static void publish_pack_untagger(PackUntagFn fn) {
        pack_untag_slot().store(fn, std::memory_order_release);
    }
    static void untag_pack_ref(xi_pack_handle f, ImagePoolOwnerId owner) {
        if (auto fn = pack_untag_slot().load(std::memory_order_acquire))
            fn(f, owner);
    }

    // Capability plane (docs/new_gen/14 pilot): the two host-owned vtables —
    // xi.cap@1 (consumer call funnel) and xi.cap.provider@1 (registration) —
    // are published here by xi::install_cap_plane (xi_cap_abi.hpp). Same
    // layering bridge as pack_iface_slot: this header cannot include
    // xi_cap_abi.hpp (which sits above it). Null until the plane is installed
    // (a host with no capability plane answers NULL for both ids).
    static std::atomic<const void*>& cap_iface_slot() {
        static std::atomic<const void*> slot{nullptr};
        return slot;
    }
    static void publish_cap_iface(const void* iface) {
        cap_iface_slot().store(iface, std::memory_order_release);
    }
    static std::atomic<const void*>& cap_provider_iface_slot() {
        static std::atomic<const void*> slot{nullptr};
        return slot;
    }
    static void publish_cap_provider_iface(const void* iface) {
        cap_provider_iface_slot().store(iface, std::memory_order_release);
    }
    // Capability-registry OWNER SWEEP — the registration analogue of
    // release_all_for / sweep_packs_for. Published by install_cap_plane; the
    // teardown paths that sweep leaked image handles + pack refs (the
    // CAbiInstanceAdapter dtor) also drop this owner's registrations, so a lib
    // plugin that forgets to unregister on destroy can never leave a dangling
    // handler in the registry. The adapter's reinit() calls it too, BEFORE the
    // rebuild factory runs: the registered `self` pointers belong to the
    // corrupted instance about to be destroyed (the fresh factory re-registers).
    // Returns the number of registrations dropped; 0 until published.
    using CapSweepFn = int (*)(ImagePoolOwnerId owner);
    static std::atomic<CapSweepFn>& cap_sweep_slot() {
        static std::atomic<CapSweepFn> slot{nullptr};
        return slot;
    }
    static void publish_cap_sweeper(CapSweepFn fn) {
        cap_sweep_slot().store(fn, std::memory_order_release);
    }
    static int sweep_caps_for(ImagePoolOwnerId owner) {
        if (auto fn = cap_sweep_slot().load(std::memory_order_acquire))
            return fn(owner);
        return 0;
    }
    // [ABI v12 — the xi.emit@1 emit_record forwarder was DELETED at THE CUT.]
    // xi.emit@1 now carries only emit_binary (the WS binary push); the Record
    // source-emit verb is replaced by xi.pack@1 emit_pack.
    static const xi_emit_v1* emit_v1_iface() {
        static const xi_emit_v1 iface = [] {
            const xi_host_api* h = canonical_host_api();
            xi_emit_v1 i{};
            i.emit_binary = h->emit_binary;
            return i;
        }();
        return &iface;
    }
    static const xi_log_v1* log_v1_iface() {
        static const xi_log_v1 iface = [] {
            const xi_host_api* h = canonical_host_api();
            xi_log_v1 i{};
            i.log        = h->log;
            i.set_status = h->set_status;
            return i;
        }();
        return &iface;
    }

    // The canonical, process-stable host table — built ONCE. It backs the carved
    // interface builders below (imaging/doc/emit/log copy their fn-pointers from
    // it, so each interface entry is byte-for-byte the same pointer as the struct
    // field). Since Phase 4 it is NO LONGER handed out through the door — the
    // xi.legacy@9 whole-table view was retired (core_fix_plan.md §12). Lazily built
    // on first use, so constructing the table itself never recurses through the door.
    static const xi_host_api* canonical_host_api() {
        static const xi_host_api api = make_host_api();
        return &api;
    }

    // The {id, version} -> const void* registry resolver wired into every
    // table's get_interface. A small, lock-free lookup (no map, no static-init
    // ordering hazard); extend with one branch per carved interface. A published
    // (id, vN) is frozen forever — a changed capability is a NEW (id, vN+1).
    static const void* get_interface_impl(const char* id, uint32_t version) {
        if (!id) return nullptr;
        // xi.legacy@9 was RETIRED in Phase 4 (core_fix_plan.md §12): the whole-
        // table legacy view is no longer published (a "stop answering a query id"
        // change, not a layout change). Callers reach capabilities via the carved
        // interfaces below or the struct fields directly. canonical_host_api()
        // still exists — it backs the carved interface builders — but it is no
        // longer handed out through the door.
        if (std::strcmp(id, "xi.preview") == 0 && version == 1)
            return preview_v1_iface();
        if (std::strcmp(id, "xi.imaging") == 0 && version == 1)
            return imaging_v1_iface();
        if (std::strcmp(id, "xi.imaging_rw") == 0 && version == 1)
            return imaging_rw_v1_iface();
        // [xi.doc@1 retired at THE CUT (v12) — Record yyjson-doc plane gone.]
        if (std::strcmp(id, "xi.emit") == 0 && version == 1)
            return emit_v1_iface();
        if (std::strcmp(id, "xi.log") == 0 && version == 1)
            return log_v1_iface();
        // polaris2 wave-2: the Pack data plane, published via publish_pack_iface
        // (slot-bridged from xi_pack_abi.hpp; null on a host with no pack plane).
        if (std::strcmp(id, "xi.pack") == 0 && version == 1)
            return pack_iface_slot().load(std::memory_order_acquire);
        // pack-v3: the slab-generation supplement to xi.pack@1 (tensor/blob/
        // adopt_bin/ordinal iteration). The version matches the container
        // generation — there was never an xi.pack@2, and 2 answers NULL.
        if (std::strcmp(id, "xi.pack") == 0 && version == 3)
            return pack3_iface_slot().load(std::memory_order_acquire);
        // Capability plane (docs/new_gen/14 pilot), published via
        // install_cap_plane (slot-bridged from xi_cap_abi.hpp; null on a host
        // with no capability plane). ZERO xi_host_api slots — both directions
        // ride this door pre-v12.
        if (std::strcmp(id, "xi.cap") == 0 && version == 1)
            return cap_iface_slot().load(std::memory_order_acquire);
        if (std::strcmp(id, "xi.cap.provider") == 0 && version == 1)
            return cap_provider_iface_slot().load(std::memory_order_acquire);
        return nullptr;
    }

    // Freeze-guard (core_fix_plan.md §12): on a FULLY WIRED table (make_host_api
    // + install_trigger_hook), assert every carved interface fn-pointer is the
    // SAME pointer as its xi_host_api struct-field twin, so the door and the field
    // can never silently drift onto different code paths. Every check below is a
    // straight pointer match. Returns true when everything tracks. Called at
    // startup on default_host_api (DEBUG assert) and by test_interface_domains.
    // Pass a table AFTER install_trigger_hook.
    static bool door_matches_fields(const xi_host_api& api) {
        if (!api.get_interface) return false;
        bool ok = true;

        const auto* pv = static_cast<const xi_preview_v1*>(api.get_interface("xi.preview", 1));
        ok = ok && pv && pv->compress == api.compress_image;

        const auto* iv = static_cast<const xi_imaging_v1*>(api.get_interface("xi.imaging", 1));
        ok = ok && iv
             && iv->image_create    == api.image_create
             && iv->image_addref    == api.image_addref
             && iv->image_release   == api.image_release
             && iv->image_data      == api.image_data
             && iv->image_width     == api.image_width
             && iv->image_height    == api.image_height
             && iv->image_channels  == api.image_channels
             && iv->image_stride    == api.image_stride;
             // [ABI v12 — read_image_file dropped from xi.imaging@1 at THE CUT.]

        // [ABI v12 — the xi.doc@1 block was DELETED at THE CUT (doc slots gone).]

        const auto* ev = static_cast<const xi_emit_v1*>(api.get_interface("xi.emit", 1));
        ok = ok && ev
             && ev->emit_binary == api.emit_binary;
             // [ABI v12 — emit_record dropped from xi.emit@1 at THE CUT.]

        const auto* lv = static_cast<const xi_log_v1*>(api.get_interface("xi.log", 1));
        ok = ok && lv
             && lv->log        == api.log
             && lv->set_status == api.set_status;

        return ok;
    }

    using DecodeImageFn = xi_image_handle (*)(const char* path);
    static DecodeImageFn& decode_image_fn() {
        static DecodeImageFn fn = nullptr;
        return fn;
    }
    static void install_decode_image(DecodeImageFn fn) {
        decode_image_fn() = fn;
    }

private:
    // Slot — one per logical handle slot.
    //
    //   entry      : atomic<PoolEntry*>. nullptr = slot is free.
    //   generation : monotonic per-slot reuse counter; new entries are
    //                stamped with the next value so stale handles fail
    //                lookup against this slot's new occupant.
    //   next_free  : free-list link, valid only when entry==nullptr.
    struct Slot {
        std::atomic<PoolEntry*> entry{nullptr};
        std::atomic<uint64_t>   generation{0};
        std::atomic<uint32_t>   next_free{0};   // 0 = list terminator
    };

    // The ONE handle-resolve primitive (round-3 W2 #5). Root cause: release()
    // re-implemented lookup()'s idx-mask / entry-load / generation-compare inline
    // because it also needs the slot index — two drifting copies of the pool's
    // most safety-critical check (both also carried a dead `idx >= SLOT_COUNT`
    // branch: idx = h & SLOT_MASK can never exceed SLOT_COUNT-1). Rejects stale
    // handles whose generation no longer matches the slot's current occupant —
    // without this a careless plugin that holds a handle past release would land
    // on the next allocation. `idx_out` (optional) receives the slot index.
    PoolEntry* lookup_(xi_image_handle h, uint32_t* idx_out) const {
        uint32_t idx = (uint32_t)(h & SLOT_MASK);
        PoolEntry* e = slots_[idx].entry.load(std::memory_order_acquire);
        if (!e) return nullptr;
        if (e->generation != ((h >> SLOT_BITS) & GEN_MAX)) return nullptr;
        if (idx_out) *idx_out = idx;
        return e;
    }

    Slot                  slots_[SLOT_COUNT];
    // High-water mark for slots never yet allocated. Slot 0 is reserved
    // (handle 0 means INVALID), so we start at 1.
    std::atomic<uint32_t> next_fresh_{1};
    // Treiber-stack head. Packed (version << 32) | slot_index; the
    // version field defends against ABA on push/pop races. 0 in the
    // low bits = list empty.
    std::atomic<uint64_t> free_head_{0};
    // Cumulative diagnostics — never decremented except `live_count_`,
    // which mirrors stats().handle_count via cheap atomics so the
    // peak watermark math doesn't have to walk the slot array.
    std::atomic<uint64_t> total_created_{0};
    std::atomic<int32_t>  live_count_{0};
    std::atomic<int32_t>  high_water_{0};

    // ---- deferred reclamation for the diagnostic slot walks -----------
    //
    // The lock-free hot path frees a PoolEntry the instant its last ref drops
    // (release / release_all_for). A diagnostic walk (stats / stats_by_owner)
    // cannot hold a ref on every slot it visits, so without coordination it can
    // dereference an entry a concurrent release just `delete`d — a UAF read
    // (external review 08 finding 1).
    //
    // Fix, entirely on the walk side so the churn path pays nothing: a walk
    // announces itself by bumping active_walkers_. reclaim_entry_ (the ONLY
    // place an entry is freed) frees inline when active_walkers_ == 0 — the
    // steady state, byte-for-byte the old behaviour bar one seq_cst load — and
    // otherwise defers the entry onto retired_, drained when the walker count
    // falls back to 0.
    //
    // Correctness rests on a StoreLoad handshake: release stores nullptr into
    // the slot (seq_cst) BEFORE reclaim_entry_ loads active_walkers_ (seq_cst),
    // and a walker bumps active_walkers_ (seq_cst) BEFORE loading the slot
    // (also seq_cst — the walker slot loads MUST be seq_cst, not merely acquire,
    // or they fall outside the total order and the argument below does not close
    // on weak-memory targets such as ARM64). The
    // seq_cst total order then guarantees: if a walker observed the entry
    // (i.e. loaded it before the slot was nulled), the releaser observes
    // active_walkers_ > 0 and defers — so a walker never frees, and never reads,
    // under its own feet. A deferred entry is freed only once active_walkers_
    // hits 0 again, by which point no walker that ever saw it is still running,
    // and no walker starting afterwards can reach it (its slot is already null).
    std::atomic<uint32_t> active_walkers_{0};
    std::mutex            retire_mu_;
    std::vector<PoolEntry*> retired_;

    // RAII: the scope of one diagnostic slot walk. Constructed by stats(),
    // stats_by_owner(), and release_all_for().
    struct WalkGuard {
        ImagePool& p_;
        explicit WalkGuard(ImagePool& p) : p_(p) {
            p_.active_walkers_.fetch_add(1, std::memory_order_seq_cst);
        }
        ~WalkGuard() {
            if (p_.active_walkers_.fetch_sub(1, std::memory_order_seq_cst) == 1)
                p_.drain_retired_();
        }
        WalkGuard(const WalkGuard&) = delete;
        WalkGuard& operator=(const WalkGuard&) = delete;
    };

    // Free e now if no diagnostic walk is in flight; otherwise defer it until
    // the last walker leaves. Called AFTER e's slot has been nulled seq_cst.
    void reclaim_entry_(PoolEntry* e) {
        if (active_walkers_.load(std::memory_order_seq_cst) == 0) {
            delete e;
            return;
        }
        {
            std::lock_guard<std::mutex> lk(retire_mu_);
            retired_.push_back(e);
        }
        // A walk may have finished between the check and the push; reclaim now
        // so a lone stats() call can't leave entries pending indefinitely.
        if (active_walkers_.load(std::memory_order_seq_cst) == 0) drain_retired_();
    }

    void drain_retired_() {
        std::vector<PoolEntry*> local;
        {
            std::lock_guard<std::mutex> lk(retire_mu_);
            local.swap(retired_);
        }
        for (auto* e : local) delete e;
    }

    uint32_t acquire_slot_() {
        // Try the free list first.
        while (true) {
            uint64_t head = free_head_.load(std::memory_order_acquire);
            uint32_t idx  = (uint32_t)(head & 0xFFFFFFFFu);
            if (idx == 0) break;            // empty — fall through
            uint32_t next = slots_[idx].next_free.load(std::memory_order_relaxed);
            uint64_t version = (head >> 32) + 1;
            uint64_t new_head = (version << 32) | (uint64_t)next;
            if (free_head_.compare_exchange_weak(head, new_head,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return idx;
            }
        }
        // No free slots — bump high water. Saturate the counter at SLOT_COUNT so a
        // pool that's already exhausted can't keep incrementing past UINT32_MAX,
        // wrap, and hand back an index that aliases a LIVE slot (which create()
        // would then overwrite). Saturated → every further alloc cleanly fails.
        uint32_t idx = next_fresh_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= SLOT_COUNT) {
            next_fresh_.store(SLOT_COUNT, std::memory_order_relaxed);
            return 0xFFFFFFFFu;   // pool exhausted
        }
        return idx;
    }

    void release_slot_(uint32_t idx) {
        while (true) {
            uint64_t head = free_head_.load(std::memory_order_acquire);
            uint32_t old_idx = (uint32_t)(head & 0xFFFFFFFFu);
            slots_[idx].next_free.store(old_idx, std::memory_order_relaxed);
            uint64_t version = (head >> 32) + 1;
            uint64_t new_head = (version << 32) | (uint64_t)idx;
            if (free_head_.compare_exchange_weak(head, new_head,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return;
            }
        }
    }
};

// RAII owner-tagging scope for instance construction. Encapsulates the
// alloc → guard-the-factory → adopt-on-success → sweep-on-failure protocol that
// EVERY instance-creation site (create_instance, rename, project-load, the reload
// paths) has to get exactly right — forgetting the adopt orphans the ctor's images
// at owner 0 (never reclaimed → leak), forgetting the sweep leaks on a failed
// ctor. Making it a single RAII type makes both impossible to forget: the images
// the factory allocates are tagged, and unless release() hands the id to the
// adapter, the destructor sweeps them.
//
// COLD PATH ONLY — used at instance create/rename/reload, never per frame. The
// hot process() path uses the adapter's already-set owner_id_ and is untouched,
// so this adds zero per-frame cost (one atomic fetch_add + a thread_local swap at
// construction, a bool branch at destruction).
class ImagePoolOwnerScope {
public:
    ImagePoolOwnerScope() : id_(ImagePool::alloc_owner_id()) {}
    ~ImagePoolOwnerScope() {
        if (released_) return;
        ImagePool::instance().release_all_for(id_);
        // Capability plane (doc 14 pilot): a factory that REGISTERED and then
        // failed construction must not leave dangling handler/self pointers in
        // the registry — sweep its registrations with its images (slot bridge;
        // no-op until install_cap_plane).
        ImagePool::sweep_caps_for(id_);
        // Pack plane: a factory that sealed an owner-tagged pack before throwing
        // would otherwise leak it — sweep the third plane too, matching the
        // adapter dtor's three-plane sweep (slot bridge; no-op until the pack
        // ABI is installed).
        ImagePool::sweep_packs_for(id_);
    }
    ImagePoolOwnerScope(const ImagePoolOwnerScope&) = delete;
    ImagePoolOwnerScope& operator=(const ImagePoolOwnerScope&) = delete;

    ImagePoolOwnerId id() const { return id_; }

    // Run the plugin factory (or anything that may allocate pool images) with
    // current_owner == id_, so its images are tagged to this scope. The guard is
    // scoped to just this call. Returns whatever fn returns.
    template <class Fn>
    auto run_factory(Fn&& fn) -> decltype(fn()) {
        ImagePool::OwnerGuard g(id_);
        return std::forward<Fn>(fn)();
    }

    // Hand the owner id off to its new long-lived owner (e.g. the adapter via
    // adopt_owner_id). After this the scope will NOT sweep — the adapter's dtor
    // owns the cleanup. Call ONLY once construction has fully succeeded.
    ImagePoolOwnerId release() { released_ = true; return id_; }

private:
    ImagePoolOwnerId id_;
    bool             released_ = false;
};

} // namespace xi
