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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace xi {

struct PoolEntry {
    std::vector<uint8_t> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int32_t channels = 0;
    std::atomic<int32_t> refcount{1};
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

        xi_image_handle handle = next_handle_++;
        auto& shard = shard_for(handle);
        {
            std::unique_lock<std::shared_mutex> lk(shard.mu);
            shard.entries[handle] = entry;
        }
        return handle;
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

    static xi_host_api make_host_api() {
        xi_host_api api = {};
        api.image_create   = [](int32_t w, int32_t h, int32_t ch) -> xi_image_handle {
            return ImagePool::instance().create(w, h, ch);
        };
        api.image_addref   = [](xi_image_handle h) { ImagePool::instance().addref(h); };
        api.image_release  = [](xi_image_handle h) { ImagePool::instance().release(h); };
        api.image_data     = [](xi_image_handle h) -> uint8_t* { return ImagePool::instance().data(h); };
        api.image_width    = [](xi_image_handle h) -> int32_t { return ImagePool::instance().width(h); };
        api.image_height   = [](xi_image_handle h) -> int32_t { return ImagePool::instance().height(h); };
        api.image_channels = [](xi_image_handle h) -> int32_t { return ImagePool::instance().channels(h); };
        api.image_stride   = [](xi_image_handle h) -> int32_t { return ImagePool::instance().stride(h); };
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
        return api;
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
