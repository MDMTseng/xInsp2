#pragma once
//
// xi_image_pool.hpp — host-side refcounted image pool (sharded).
//
// 16 shards, each with its own shared_mutex. Handles are distributed
// across shards by hash, so concurrent create/release/data calls from
// different threads rarely contend on the same lock.
//
// Performance:
//   - data/width/height/channels: shared_lock on ONE shard (concurrent reads)
//   - addref: shared_lock + atomic increment (concurrent)
//   - create: unique_lock on ONE shard (16x less contention than single lock)
//   - release (refcount→0): unique_lock on ONE shard, delete outside lock
//
// Thread safety guarantee:
//   - Safe for xi::async parallel tasks
//   - Safe for multiple camera threads pushing simultaneously
//   - Safe for concurrent plugin process() calls
//

#include "xi_abi.h"
#include "xi_image.hpp"
#include "xi_instance_folders.hpp"
#include "xi_shm.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace xi {

// Per-creator identity. Lets the pool sweep all handles allocated on
// behalf of a given plugin instance / script when that owner dies
// (instance destroyed, worker process exited, script unloaded). An
// owner of 0 means "anonymous / framework" — handles created with no
// owner context (e.g. the backend's own grab path) are never swept.
//
// Owner IDs are uint32, allocated monotonically by alloc_owner_id().
// Two instances created and destroyed at different times will have
// different IDs even if their names match — sweeps target the
// specific live-instance.
using ImagePoolOwnerId = uint32_t;

struct PoolEntry {
    std::vector<uint8_t> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 0;
    std::atomic<int32_t> refcount{1};
    ImagePoolOwnerId owner = 0;     // who allocated this; 0 = anonymous
};

class ImagePool {
    static constexpr int SHARD_COUNT = 16;
    static constexpr int SHARD_MASK  = SHARD_COUNT - 1;

    struct Shard {
        std::shared_mutex mu;
        std::unordered_map<xi_image_handle, PoolEntry*> entries;
    };

public:
    static ImagePool& instance() {
        static ImagePool pool;
        return pool;
    }

    xi_image_handle create(int32_t w, int32_t h, int32_t ch) {
        auto* entry = new PoolEntry();
        entry->pixels.resize((size_t)w * h * ch);
        entry->width = w;
        entry->height = h;
        entry->channels = ch;
        entry->refcount = 1;
        // Tag with whoever's running on this thread right now. 0 if
        // no OwnerGuard is active (host-internal allocations are
        // anonymous and therefore never swept).
        entry->owner = current_owner();

        xi_image_handle handle = next_handle_++;
        auto& shard = shard_for(handle);
        {
            std::unique_lock<std::shared_mutex> lk(shard.mu);
            shard.entries[handle] = entry;
        }
        return handle;
    }

    // ---- Owner ledger ----
    //
    // alloc_owner_id() — give an instance / script a unique non-zero id.
    // OwnerGuard — RAII install/restore of the per-thread current owner.
    // release_all_for(id) — sweep every handle tagged with `id` and
    //   release once each. Used on instance destroy / worker death /
    //   script unload to clean up handles that the dying party held.
    // stats(id) — for cmd:image_pool_stats: count + total bytes per
    //   owner (or all owners if id == 0).
    //
    // The thread-local current_owner is process-wide (via a stable
    // accessor), so it survives DLL reloads.

    static ImagePoolOwnerId alloc_owner_id() {
        // Start at 1; 0 reserved for anonymous.
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
        if (owner == 0) return 0;   // never sweep anonymous
        int swept = 0;
        for (int i = 0; i < SHARD_COUNT; ++i) {
            auto& shard = shards_[i];
            std::vector<PoolEntry*> to_delete;
            std::vector<xi_image_handle> erased;
            {
                std::unique_lock<std::shared_mutex> lk(shard.mu);
                for (auto it = shard.entries.begin(); it != shard.entries.end();) {
                    if (it->second->owner == owner) {
                        // Force release regardless of refcount — the
                        // owner is gone and no further consumers can
                        // legitimately hold a ref.
                        to_delete.push_back(it->second);
                        erased.push_back(it->first);
                        it = shard.entries.erase(it);
                        ++swept;
                    } else {
                        ++it;
                    }
                }
            }
            for (auto* e : to_delete) delete e;
            (void)erased;
        }
        return swept;
    }

    struct OwnerStats {
        int      handle_count = 0;
        uint64_t total_bytes  = 0;
    };
    // owner == 0 → aggregate across all owners (including anonymous).
    OwnerStats stats(ImagePoolOwnerId owner = 0) {
        OwnerStats s{};
        for (int i = 0; i < SHARD_COUNT; ++i) {
            auto& shard = shards_[i];
            std::shared_lock<std::shared_mutex> lk(shard.mu);
            for (auto& [_, e] : shard.entries) {
                if (owner != 0 && e->owner != owner) continue;
                ++s.handle_count;
                s.total_bytes += e->pixels.size();
            }
        }
        return s;
    }

    // For cmd:image_pool_stats: list every owner's footprint. Returns
    // a vector of (owner_id, count, bytes); owner_id == 0 means
    // anonymous.
    struct PerOwnerStat {
        ImagePoolOwnerId owner = 0;
        int              handle_count = 0;
        uint64_t         total_bytes  = 0;
    };
    std::vector<PerOwnerStat> stats_by_owner() {
        std::unordered_map<ImagePoolOwnerId, PerOwnerStat> agg;
        for (int i = 0; i < SHARD_COUNT; ++i) {
            auto& shard = shards_[i];
            std::shared_lock<std::shared_mutex> lk(shard.mu);
            for (auto& [_, e] : shard.entries) {
                auto& s = agg[e->owner];
                s.owner = e->owner;
                ++s.handle_count;
                s.total_bytes += e->pixels.size();
            }
        }
        std::vector<PerOwnerStat> out;
        out.reserve(agg.size());
        for (auto& [_, s] : agg) out.push_back(s);
        return out;
    }

    void addref(xi_image_handle h) {
        auto& shard = shard_for(h);
        std::shared_lock<std::shared_mutex> lk(shard.mu);
        auto it = shard.entries.find(h);
        if (it != shard.entries.end()) {
            it->second->refcount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void release(xi_image_handle h) {
        PoolEntry* to_delete = nullptr;
        auto& shard = shard_for(h);
        {
            std::unique_lock<std::shared_mutex> lk(shard.mu);
            auto it = shard.entries.find(h);
            if (it == shard.entries.end()) return;
            if (it->second->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                to_delete = it->second;
                shard.entries.erase(it);
            }
        }
        delete to_delete;
    }

    uint8_t* data(xi_image_handle h) {
        auto& shard = shard_for(h);
        std::shared_lock<std::shared_mutex> lk(shard.mu);
        auto it = shard.entries.find(h);
        return (it != shard.entries.end()) ? it->second->pixels.data() : nullptr;
    }

    int32_t width(xi_image_handle h) {
        auto& shard = shard_for(h);
        std::shared_lock<std::shared_mutex> lk(shard.mu);
        auto it = shard.entries.find(h);
        return (it != shard.entries.end()) ? it->second->width : 0;
    }

    int32_t height(xi_image_handle h) {
        auto& shard = shard_for(h);
        std::shared_lock<std::shared_mutex> lk(shard.mu);
        auto it = shard.entries.find(h);
        return (it != shard.entries.end()) ? it->second->height : 0;
    }

    int32_t channels(xi_image_handle h) {
        auto& shard = shard_for(h);
        std::shared_lock<std::shared_mutex> lk(shard.mu);
        auto it = shard.entries.find(h);
        return (it != shard.entries.end()) ? it->second->channels : 0;
    }

    int32_t stride(xi_image_handle h) {
        auto& shard = shard_for(h);
        std::shared_lock<std::shared_mutex> lk(shard.mu);
        auto it = shard.entries.find(h);
        return (it != shard.entries.end()) ? it->second->width * it->second->channels : 0;
    }

    xi_image_handle from_image(const Image& img) {
        if (img.empty()) return XI_IMAGE_NULL;
        auto h = create(img.width, img.height, img.channels);
        std::memcpy(data(h), img.data(), img.size());
        return h;
    }

    Image to_image(xi_image_handle h) {
        auto& shard = shard_for(h);
        std::shared_lock<std::shared_mutex> lk(shard.mu);
        auto it = shard.entries.find(h);
        if (it == shard.entries.end()) return {};
        auto* e = it->second;
        return Image(e->width, e->height, e->channels, e->pixels.data());
    }

    // Backend wires this once at startup (post-create). Plugins / scripts
    // see it via host_api->shm_create_image. nullptr = no SHM available;
    // shm_create_image / shm_alloc_buffer in host_api will then return 0.
    // Stored as a raw pointer because ShmRegion lifetime is owned by the
    // backend and outlives any plugin DLL.
    static ShmRegion*& shm_region_singleton() {
        static ShmRegion* s = nullptr;
        return s;
    }
    static void set_shm_region(ShmRegion* r) { shm_region_singleton() = r; }

    // SHM-tagged handles have 0xA5 in the top byte (see xi_shm.hpp).
    // Heap-pool handles are small uint32 values, never reaching the top.
    static bool is_shm_handle(xi_image_handle h) {
        return ((h >> 56) & 0xFF) == 0xA5;
    }

    static xi_host_api make_host_api() {
        xi_host_api api = {};
        api.image_create   = [](int32_t w, int32_t h, int32_t ch) -> xi_image_handle {
            return ImagePool::instance().create(w, h, ch);
        };
        // image_addref / release / data / width etc. dispatch to whichever
        // pool the handle came from. This lets plugins mix SHM-backed and
        // heap-backed images freely; the high-bit tag tells us which.
        api.image_addref   = [](xi_image_handle h) {
            if (is_shm_handle(h)) { auto* r = shm_region_singleton(); if (r) r->addref(h); }
            else                    ImagePool::instance().addref(h);
        };
        api.image_release  = [](xi_image_handle h) {
            if (is_shm_handle(h)) { auto* r = shm_region_singleton(); if (r) r->release(h); }
            else                    ImagePool::instance().release(h);
        };
        api.image_data     = [](xi_image_handle h) -> uint8_t* {
            if (is_shm_handle(h)) { auto* r = shm_region_singleton(); return r ? r->data(h) : nullptr; }
            return ImagePool::instance().data(h);
        };
        api.image_width    = [](xi_image_handle h) -> int32_t {
            if (is_shm_handle(h)) { auto* r = shm_region_singleton(); return r ? r->width(h) : 0; }
            return ImagePool::instance().width(h);
        };
        api.image_height   = [](xi_image_handle h) -> int32_t {
            if (is_shm_handle(h)) { auto* r = shm_region_singleton(); return r ? r->height(h) : 0; }
            return ImagePool::instance().height(h);
        };
        api.image_channels = [](xi_image_handle h) -> int32_t {
            if (is_shm_handle(h)) { auto* r = shm_region_singleton(); return r ? r->channels(h) : 0; }
            return ImagePool::instance().channels(h);
        };
        api.image_stride   = [](xi_image_handle h) -> int32_t {
            // SHM images store contiguous pixels: stride = w * channels.
            if (is_shm_handle(h)) {
                auto* r = shm_region_singleton();
                return r ? r->width(h) * r->channels(h) : 0;
            }
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
        // emit_trigger is wired by xi::install_trigger_hook(api) in
        // xi_trigger_bus.hpp — defaults to a no-op so plugins linked
        // against an older host don't crash.
        api.emit_trigger = nullptr;

        // SHM extensions. Return 0 (INVALID_HANDLE) when no SHM region is
        // installed — plugins are expected to fall back to image_create.
        api.shm_create_image = [](int32_t w, int32_t h, int32_t ch) -> xi_image_handle {
            auto* r = shm_region_singleton();
            return r ? r->alloc_image(w, h, ch) : 0;
        };
        api.shm_alloc_buffer = [](int32_t size_bytes) -> xi_image_handle {
            auto* r = shm_region_singleton();
            return r ? r->alloc_buffer(size_bytes) : 0;
        };
        // shm_addref / release route through the same per-handle dispatch
        // as image_addref / release above, but exposed separately so a
        // plugin that ALWAYS wants the SHM path can skip the tag check.
        api.shm_addref  = [](xi_image_handle h) {
            auto* r = shm_region_singleton(); if (r) r->addref(h);
        };
        api.shm_release = [](xi_image_handle h) {
            auto* r = shm_region_singleton(); if (r) r->release(h);
        };
        api.shm_is_shm_handle = [](xi_image_handle h) -> int32_t {
            return is_shm_handle(h) ? 1 : 0;
        };
        // read_image_file is wired by backend / worker / runner via
        // install_read_image_file (linked from xi_image_io.cpp). Test
        // executables that don't link the io TU leave it null —
        // callers MUST null-check.
        api.read_image_file = read_image_file_fn();
        return api;
    }

    // Pluggable file-decoder. Defaults to null (returns 0 / no
    // decoder). Backend, worker, runner each link xi_image_io.cpp
    // which calls install_read_image_file() at static-init time.
    using ReadImageFileFn = xi_image_handle (*)(const char* path);
    static ReadImageFileFn& read_image_file_fn() {
        static ReadImageFileFn fn = nullptr;
        return fn;
    }
    static void install_read_image_file(ReadImageFileFn fn) {
        read_image_file_fn() = fn;
    }

private:
    Shard shards_[SHARD_COUNT];
    // 64-bit counter truncated to 32-bit handle. At 1M handles/sec it takes
    // 585 years to wrap uint64, so ABA is practically impossible.
    std::atomic<uint64_t> next_handle_{1};

    Shard& shard_for(xi_image_handle h) {
        return shards_[(int)(h & SHARD_MASK)];
    }
};

} // namespace xi
