#pragma once
//
// xi_image_pool.hpp — host-side refcounted image pool (lock-free).
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
#include "xi_doc_pool.hpp"      // γ: backs host_api.doc_chunk_* (pooled doc allocator)
#include "xi_doc_registry.hpp"  // γ-4: backs host_api.doc_retain/doc_release

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xi {

// True while the ImagePool singleton is alive. Namespace-scope (constant-
// initialised, trivial dtor) so it is valid before the singleton is constructed
// and remains readable after it is destroyed at process exit — letting late
// teardown paths (e.g. a deferred LoadedScript module_lifetime deleter that runs
// during static destruction) skip pool access once the pool is gone instead of
// touching a destroyed Meyers singleton (UB).
inline std::atomic<bool> g_image_pool_alive{false};

// Per-creator identity. Lets the pool sweep all handles allocated on
// behalf of a given plugin instance / script when that owner dies
// (instance destroyed, worker process exited, script unloaded). An
// owner of 0 means "anonymous / framework" — handles created with no
// owner context (e.g. the backend's own grab path) are never swept.
using ImagePoolOwnerId = uint32_t;

struct PoolEntry {
    std::vector<uint8_t> pixels;
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
};

class ImagePool {
public:
    static constexpr uint32_t SLOT_BITS  = 16;
    static constexpr uint32_t SLOT_COUNT = 1u << SLOT_BITS;   // 65 536
    static constexpr uint64_t SLOT_MASK  = SLOT_COUNT - 1;
    // Max generation that fits in (64 - 8 - SLOT_BITS) = 40 bits.
    static constexpr uint64_t GEN_MAX    = (1ull << 40) - 1;

    static ImagePool& instance() {
        static ImagePool pool;
        g_image_pool_alive.store(true, std::memory_order_release);
        return pool;
    }

    ~ImagePool() { g_image_pool_alive.store(false, std::memory_order_release); }

    // ---- core lookup -------------------------------------------------

    PoolEntry* lookup(xi_image_handle h) const {
        uint32_t idx = (uint32_t)(h & SLOT_MASK);
        if (idx >= SLOT_COUNT) return nullptr;
        PoolEntry* e = slots_[idx].entry.load(std::memory_order_acquire);
        if (!e) return nullptr;
        // Reject stale handles whose generation no longer matches the
        // slot's current occupant. Without this a careless plugin that
        // holds a handle past release would land on the next allocation.
        if (e->generation != ((h >> SLOT_BITS) & GEN_MAX)) return nullptr;
        return e;
    }

    // ---- create / release -------------------------------------------

    xi_image_handle create(int32_t w, int32_t h, int32_t ch) {
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
        try {
            entry->pixels.resize((size_t)pixels);
        } catch (const std::bad_alloc&) {
            // entry deletes via unique_ptr; counters untouched.
            return 0;
        }
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

    void addref(xi_image_handle h) {
        if (auto* e = lookup(h)) {
            e->refcount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void release(xi_image_handle h) {
        uint32_t idx = (uint32_t)(h & SLOT_MASK);
        if (idx >= SLOT_COUNT) return;
        PoolEntry* e = slots_[idx].entry.load(std::memory_order_acquire);
        if (!e) return;
        if (e->generation != ((h >> SLOT_BITS) & GEN_MAX)) return;
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
        return e ? e->pixels.data() : nullptr;
    }

    // Read-only pixel pointer — the blessed accessor for ANY handle (input or
    // output). Same bytes data() returns, const-qualified so it can never be a
    // mutation path. Backs xi.imaging_rw@1.image_read. Null on a bad handle.
    const uint8_t* read_data(xi_image_handle h) {
        auto* e = lookup(h);
        return e ? e->pixels.data() : nullptr;
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
        return e->pixels.data();
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
        return Image(e->width, e->height, e->channels, e->pixels.data());
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
            PoolEntry* e = slots_[i].entry.load(std::memory_order_acquire);
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

    struct OwnerStats {
        int      handle_count = 0;
        uint64_t total_bytes  = 0;
    };
    OwnerStats stats(ImagePoolOwnerId owner = 0) {
        // WalkGuard makes this read-only slot walk memory-safe against a
        // concurrent release(): while any walker is live, a last-ref release
        // defers its `delete` to the retire list instead of freeing under our
        // pointer, so `e->pixels.size()` here can never touch a freed entry
        // (external review 08 finding 1). Zero cost when no walk is in flight.
        WalkGuard wg(*this);
        OwnerStats s{};
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            PoolEntry* e = slots_[i].entry.load(std::memory_order_acquire);
            if (!e) continue;
            if (owner != 0 && e->owner.load(std::memory_order_relaxed) != owner) continue;
            ++s.handle_count;
            s.total_bytes += e->pixels.size();
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
            PoolEntry* e = slots_[i].entry.load(std::memory_order_acquire);
            if (!e) continue;
            ImagePoolOwnerId ow = e->owner.load(std::memory_order_relaxed);
            auto& s = agg[ow];
            s.owner = ow;
            ++s.handle_count;
            s.total_bytes += e->pixels.size();
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
        // ABI v6: wired by install_trigger_hook (xi_trigger_bus.hpp); null here
        // so a host that never installs the hook leaves it null (the SDK helper
        // null-checks).
        api.emit_record = nullptr;
        // (ABI v11: the five always-null shm_* fields were removed from
        // xi_host_api in Phase 4 — nothing to null here anymore. SHM was
        // removed 2026-05; plugins use image_create / the host ImagePool,
        // zero-copy via pointers within the single backend process.)
        api.read_image_file = read_image_file_fn();
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
        // Host doc allocator (ABI v3, γ) — backs the in-process yyjson doc
        // pass-by-pointer path. A doc built through these is host-owned, so its
        // free routes back to the host and is safe to drop from either side of
        // the DLL boundary. Backed by DocChunkPool (γ-5): a thread-local
        // size-class free-list ⇒ no per-frame malloc churn on the hot path.
        api.doc_chunk_alloc   = [](size_t n) -> void* { return xi::DocChunkPool::alloc(n); };
        api.doc_chunk_realloc = [](void* p, size_t n) -> void* { return xi::DocChunkPool::realloc(p, n); };
        api.doc_chunk_free    = [](void* p) { xi::DocChunkPool::free(p); };
        // Host doc refcount (ABI v4, γ-4) — lets a yyjson doc handed across the
        // ABI be held by more than one side without a deep copy (the doc
        // analogue of image_addref/image_release).
        api.doc_retain  = [](void* d) { xi::DocRegistry::instance().addref((yyjson_mut_doc*)d); };
        api.doc_release = [](void* d) { xi::DocRegistry::instance().release((yyjson_mut_doc*)d); };
        api.doc_refcount = [](void* d) -> int32_t {
            return (int32_t)xi::DocRegistry::instance().refcount((yyjson_mut_doc*)d);
        };
        // ABI v10: the capability-query door. A plugin resolves a frozen,
        // segregated interface by id+version through this one pointer
        // (core_fix_plan.md §12 Phase 1). Registrations live in
        // get_interface_impl: the carved xi.preview/imaging/doc/emit/log@1
        // interfaces (xi.legacy@9 was retired in Phase 4).
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
            i.read_image_file = h->read_image_file;
            return i;
        }();
        return &iface;
    }
    static const xi_doc_v1* doc_v1_iface() {
        static const xi_doc_v1 iface = [] {
            const xi_host_api* h = canonical_host_api();
            xi_doc_v1 i{};
            i.doc_chunk_alloc   = h->doc_chunk_alloc;
            i.doc_chunk_realloc = h->doc_chunk_realloc;
            i.doc_chunk_free    = h->doc_chunk_free;
            i.doc_retain        = h->doc_retain;
            i.doc_release       = h->doc_release;
            i.doc_refcount      = h->doc_refcount;
            return i;
        }();
        return &iface;
    }
    // xi.emit@1 emit_record wiring bridge (ABI v6/v11 — core_fix_plan.md §12).
    //
    // emit_record is the ONE host verb not wired in make_host_api(): it is
    // installed later by install_trigger_hook (xi_trigger_bus.hpp) onto the
    // per-load table (default_host_api). image_pool.hpp CANNOT include
    // trigger_bus.hpp (layering), so the carved xi.emit@1 interface cannot copy
    // the wired pointer at build time — when emit_v1_iface() is first built the
    // hook may not have run, and its Meyers singleton is frozen thereafter. The
    // old code copied h->emit_record from the canonical table (never hooked), so
    // the door's emit_record was permanently null while the struct field was live
    // — a dormant landmine for any plugin that trusts the door.
    //
    // Bridge: install_trigger_hook publishes the wired emit_record fn-pointer into
    // this process-global, lock-free slot; the door's emit_record is a tiny stable
    // forwarder that reads the slot on each call. So the door reaches the EXACT
    // SAME wired dispatch path as the struct field host->emit_record — a live door
    // entry, never null. (The freeze-guard asserts slot == wired field.)
    using EmitRecordFn = void (*)(const char* emitter, xi_trigger_id id,
                                  const struct xi_record* rec, int64_t ts);
    static std::atomic<EmitRecordFn>& emit_record_slot() {
        static std::atomic<EmitRecordFn> slot{nullptr};
        return slot;
    }
    // Published by install_trigger_hook once the bus dispatch lambda exists.
    static void publish_emit_record(EmitRecordFn fn) {
        emit_record_slot().store(fn, std::memory_order_release);
    }
    // The forwarder the door hands out for xi.emit@1.emit_record: a stable address
    // (a plugin may cache the interface) that reads the published slot each call.
    // No-op until the hook publishes — same null-safe contract as the field.
    static void emit_record_forwarder(const char* emitter, xi_trigger_id id,
                                      const struct xi_record* rec, int64_t ts) {
        if (auto fn = emit_record_slot().load(std::memory_order_acquire))
            fn(emitter, id, rec, ts);
    }
    static const xi_emit_v1* emit_v1_iface() {
        static const xi_emit_v1 iface = [] {
            const xi_host_api* h = canonical_host_api();
            xi_emit_v1 i{};
            // emit_record: hand out the STABLE forwarder, NOT the raw field (which
            // is null on the never-hooked canonical table). The forwarder reads the
            // slot install_trigger_hook publishes, so the door reaches the same
            // wired dispatch path as host->emit_record.
            i.emit_record = &emit_record_forwarder;
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
        if (std::strcmp(id, "xi.doc") == 0 && version == 1)
            return doc_v1_iface();
        if (std::strcmp(id, "xi.emit") == 0 && version == 1)
            return emit_v1_iface();
        if (std::strcmp(id, "xi.log") == 0 && version == 1)
            return log_v1_iface();
        return nullptr;
    }

    // Freeze-guard (core_fix_plan.md §12): on a FULLY WIRED table (make_host_api
    // + install_trigger_hook), assert every carved interface fn-pointer is the
    // SAME pointer as its xi_host_api struct-field twin, so the door and the field
    // can never silently drift onto different code paths. emit_record is the one
    // FUNCTIONAL (not pointer) match: the door hands out a stable forwarder, so we
    // check the published slot equals the wired field instead. Returns true when
    // everything tracks. Called at startup on default_host_api (DEBUG assert) and
    // by test_interface_domains. Pass a table AFTER install_trigger_hook.
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
             && iv->image_stride    == api.image_stride
             && iv->read_image_file == api.read_image_file;

        const auto* dv = static_cast<const xi_doc_v1*>(api.get_interface("xi.doc", 1));
        ok = ok && dv
             && dv->doc_chunk_alloc   == api.doc_chunk_alloc
             && dv->doc_chunk_realloc == api.doc_chunk_realloc
             && dv->doc_chunk_free    == api.doc_chunk_free
             && dv->doc_retain        == api.doc_retain
             && dv->doc_release       == api.doc_release
             && dv->doc_refcount      == api.doc_refcount;

        const auto* ev = static_cast<const xi_emit_v1*>(api.get_interface("xi.emit", 1));
        ok = ok && ev
             && ev->emit_binary == api.emit_binary
             // emit_record: the door is a stable, non-null forwarder whose target
             // (the published slot) must equal the wired struct field.
             && ev->emit_record != nullptr
             && emit_record_slot().load(std::memory_order_acquire) == api.emit_record;

        const auto* lv = static_cast<const xi_log_v1*>(api.get_interface("xi.log", 1));
        ok = ok && lv
             && lv->log        == api.log
             && lv->set_status == api.set_status;

        return ok;
    }

    using ReadImageFileFn = xi_image_handle (*)(const char* path);
    static ReadImageFileFn& read_image_file_fn() {
        static ReadImageFileFn fn = nullptr;
        return fn;
    }
    static void install_read_image_file(ReadImageFileFn fn) {
        read_image_file_fn() = fn;
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
    // and a walker bumps active_walkers_ (seq_cst) BEFORE loading the slot. The
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
    ~ImagePoolOwnerScope() { if (!released_) ImagePool::instance().release_all_for(id_); }
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
