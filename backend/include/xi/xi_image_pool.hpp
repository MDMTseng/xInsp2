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
    ImagePoolOwnerId owner = 0;     // who allocated this; 0 = anonymous
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
        entry->owner    = current_owner();

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
            // Last ref — clear slot, return to free list, delete entry.
            slots_[idx].entry.store(nullptr, std::memory_order_release);
            release_slot_(idx);
            delete e;
            live_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    // ---- queries (lock-free) -----------------------------------------

    uint8_t* data(xi_image_handle h) {
        auto* e = lookup(h);
        return e ? e->pixels.data() : nullptr;
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
        int swept = 0;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            PoolEntry* e = slots_[i].entry.load(std::memory_order_acquire);
            if (!e || e->owner != owner) continue;
            // Drop P's ownership ref — same accounting as release().
            if (e->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                // Sole holder (leak): clear slot, return to free list, delete.
                slots_[i].entry.store(nullptr, std::memory_order_release);
                release_slot_(i);
                delete e;
                ++swept;
            } else {
                // A live external consumer still references this entry. Orphan
                // it (anonymous owner) so it outlives P and is freed by its last
                // holder; leave the slot + generation intact so that handle
                // still resolves.
                e->owner = 0;
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
        OwnerStats s{};
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            PoolEntry* e = slots_[i].entry.load(std::memory_order_acquire);
            if (!e) continue;
            if (owner != 0 && e->owner != owner) continue;
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
        std::unordered_map<ImagePoolOwnerId, PerOwnerStat> agg;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            PoolEntry* e = slots_[i].entry.load(std::memory_order_acquire);
            if (!e) continue;
            auto& s = agg[e->owner];
            s.owner = e->owner;
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
        // SHM removed 2026-05 (FE/BE in-process split): no shared-memory
        // region exists. Per the ABI contract these stay null and plugins
        // fall back to image_create / the host ImagePool (zero-copy via
        // pointers within the single backend process).
        api.shm_create_image  = nullptr;
        api.shm_alloc_buffer  = nullptr;
        api.shm_addref        = nullptr;
        api.shm_release       = nullptr;
        api.shm_is_shm_handle = nullptr;
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
        api.doc_retain  = [](void* d) { xi::DocRegistry::instance().retain((yyjson_mut_doc*)d); };
        api.doc_release = [](void* d) { xi::DocRegistry::instance().release((yyjson_mut_doc*)d); };
        api.doc_refcount = [](void* d) -> int32_t {
            return (int32_t)xi::DocRegistry::instance().refcount((yyjson_mut_doc*)d);
        };
        // ABI v10: the capability-query door. A plugin resolves a frozen,
        // segregated interface by id+version through this one pointer
        // (core_fix_plan.md §12 Phase 1). Registrations live in
        // get_interface_impl: "xi.legacy"@9 (the whole table) + "xi.preview"@1.
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

    // The canonical, process-stable host table — built ONCE. get_interface
    // ("xi.legacy", 9) hands this back so a caller can reach the whole v9
    // surface through the door (core_fix_plan.md §12 Phase 1: "register the
    // entire current host table as xi.legacy@9"). Lazily built on first query,
    // so constructing the table itself never recurses through the door.
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
        if (std::strcmp(id, "xi.legacy") == 0 && version == 9)
            return canonical_host_api();        // the whole v9 surface, one pointer
        if (std::strcmp(id, "xi.preview") == 0 && version == 1)
            return preview_v1_iface();
        return nullptr;
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
