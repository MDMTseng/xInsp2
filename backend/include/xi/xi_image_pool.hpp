#pragma once
//
// xi_image_pool.hpp — host-side refcounted image pool.
//
// Implements the xi_host_api image functions. All image memory lives
// here. Plugins get handles, call addref/release, access data via the
// host API function pointers.
//

#include "xi_abi.h"
#include "xi_image.hpp"

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
public:
    static ImagePool& instance() {
        static ImagePool pool;
        return pool;
    }

    xi_image_handle create(int32_t w, int32_t h, int32_t ch) {
        auto entry = new PoolEntry();
        entry->pixels.resize((size_t)w * h * ch);
        entry->width = w;
        entry->height = h;
        entry->channels = ch;
        entry->refcount = 1;
        xi_image_handle handle = next_handle_++;
        {
            std::unique_lock<std::shared_mutex> lk(mu_);
            entries_[handle] = entry;
        }
        return handle;
    }

    void addref(xi_image_handle h) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = entries_.find(h);
        if (it != entries_.end()) {
            it->second->refcount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void release(xi_image_handle h) {
        PoolEntry* to_delete = nullptr;
        {
            std::unique_lock<std::shared_mutex> lk(mu_);
            auto it = entries_.find(h);
            if (it == entries_.end()) return;
            if (it->second->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                to_delete = it->second;
                entries_.erase(it);
            }
        }
        // Delete outside the lock — pixel buffer can be large
        delete to_delete;
    }

    // Data access — holds a shared lock for the lookup, then the pointer
    // is safe as long as the caller holds a refcount (which they must,
    // since they have the handle).
    uint8_t* data(xi_image_handle h) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = entries_.find(h);
        return (it != entries_.end()) ? it->second->pixels.data() : nullptr;
    }

    int32_t width(xi_image_handle h) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = entries_.find(h);
        return (it != entries_.end()) ? it->second->width : 0;
    }

    int32_t height(xi_image_handle h) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = entries_.find(h);
        return (it != entries_.end()) ? it->second->height : 0;
    }

    int32_t channels(xi_image_handle h) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = entries_.find(h);
        return (it != entries_.end()) ? it->second->channels : 0;
    }

    int32_t stride(xi_image_handle h) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = entries_.find(h);
        return (it != entries_.end()) ? it->second->width * it->second->channels : 0;
    }

    xi_image_handle from_image(const Image& img) {
        if (img.empty()) return XI_IMAGE_NULL;
        auto h = create(img.width, img.height, img.channels);
        std::memcpy(data(h), img.data(), img.size());
        return h;
    }

    Image to_image(xi_image_handle h) {
        std::shared_lock<std::shared_mutex> lk(mu_);
        auto it = entries_.find(h);
        if (it == entries_.end()) return {};
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
        return api;
    }

private:
    std::shared_mutex mu_;
    std::unordered_map<xi_image_handle, PoolEntry*> entries_;
    std::atomic<xi_image_handle> next_handle_{1};
};

} // namespace xi
