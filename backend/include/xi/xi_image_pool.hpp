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
// Handle layout (heap pool — top byte 0; SHM handles use 0xA5 there):
//   bits 0-15   slot index   (65 536 slots)
//   bits 16-55  generation   (1 trillion reuses per slot)
//   bits 56-63  reserved 0   (kept distinct from SHM's 0xA5 tag)
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

#include <atomic>
#include <cstdio>
#include <cstring>
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

    int release_all_for(ImagePoolOwnerId owner) {
        if (owner == 0) return 0;
        int swept = 0;
        for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
            PoolEntry* e = slots_[i].entry.load(std::memory_order_acquire);
            if (!e || e->owner != owner) continue;
            // Force-release this slot regardless of refcount — owner
            // is gone, no remaining consumer is legitimate.
            slots_[i].entry.store(nullptr, std::memory_order_release);
            release_slot_(i);
            delete e;
            ++swept;
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
            std::fprintf(stderr, "[%s] %s\n", lvl[level & 3], msg);
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
        api.emit_trigger = nullptr;
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
        return api;
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
        // No free slots — bump high water.
        uint32_t idx = next_fresh_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= SLOT_COUNT) return 0xFFFFFFFFu;   // pool exhausted
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

} // namespace xi
