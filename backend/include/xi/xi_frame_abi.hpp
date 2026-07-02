#pragma once
//
// xi_frame_abi.hpp — the HOST side of the xi.frame@1 data-plane door (polaris2
// wave-2, docs/new_gen/07-uniform-keyed-buffer-plane.md + 08 Wave 2).
//
// The Frame value type (xi_frame.hpp) is a C++ container that OWNS an arena and
// pool handles; a plugin in another DLL cannot touch its layout. So the frame
// crosses the ABI as an OPAQUE HANDLE (xi_frame_handle) plus the accessor C
// functions in `xi_frame_v1` (xi_abi.h) — spans in / spans out, never raw
// struct layout (doc 02 r1). This header is where those C functions live:
//
//   * FrameRegistry — the handle table. Maps a handle -> a sealed, refcounted
//     xi::Frame (the pool-handle mint path stays host-side), and a builder
//     handle -> an xi::FrameBuilder under construction. Handles are minted ONLY
//     here (doc 07 ingress: the domain's own allocator).
//   * frame_v1_iface() — the process-stable xi_frame_v1 a plugin resolves via
//     host->get_interface("xi.frame", 1). Published into ImagePool's frame slot
//     (the same slot-bridge the emit_record door uses, since image_pool.hpp
//     can't include this header) by install_frame_abi().
//   * emit_frame — a source hands a sealed frame to dispatch; forwarded to
//     TriggerBus::emit_frame (the Record emit path is untouched).
//
// This header sits ABOVE image_pool/trigger_bus in the layering (it includes
// the container). Include it in a .cpp (the backend service, the frame tests),
// never from image_pool.hpp.
//

#include "xi_abi.h"          // xi_frame_handle / xi_frame_v1 / xi_frame_image
#include "xi_frame.hpp"      // xi::Frame / xi::FrameBuilder (the container)
#include "xi_image_pool.hpp" // ImagePool::publish_frame_iface (the door slot)
#include "xi_trigger_bus.hpp"// TriggerBus::emit_frame / set_frame_releaser

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

namespace xi {

// ===================================================================
// FrameRegistry — the opaque-handle table behind xi.frame@1.
//
// A sealed frame is single-owner in C++ but refcounted across the ABI (an event
// on the dispatch queue and the emitter can each hold a ref, exactly as image
// handles do). So the registry stores each frame with a small refcount and
// destroys it (releasing its pool handles) on the last release. Nodes of a
// std::unordered_map are pointer-stable, so a Frame* handed to an accessor stays
// valid across concurrent insert/erase of OTHER handles — the caller holds a ref
// on its own handle, so that entry cannot vanish under it.
// ===================================================================
class FrameRegistry {
public:
    static FrameRegistry& instance() {
        static FrameRegistry r;
        return r;
    }

    // ---- builder side (produce) --------------------------------------------
    xi_frame_builder new_builder() {
        uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mu_);
        builders_.emplace(id, std::make_unique<FrameBuilder>());
        return id;
    }
    FrameBuilder* builder(xi_frame_builder b) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = builders_.find(b);
        return it == builders_.end() ? nullptr : it->second.get();
    }
    // Seal + consume: move the builder's frame into the frame table (refcount 1),
    // erase the builder. XI_FRAME_NULL on a bad/dead builder id.
    xi_frame_handle seal(xi_frame_builder b) {
        std::unique_ptr<FrameBuilder> fb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = builders_.find(b);
            if (it == builders_.end()) return XI_FRAME_NULL;
            fb = std::move(it->second);
            builders_.erase(it);
        }
        uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mu_);
        frames_.emplace(id, Slot{fb->seal(), 1});
        return id;
    }
    void abandon(xi_frame_builder b) {
        std::lock_guard<std::mutex> lk(mu_);
        builders_.erase(b);   // ~FrameBuilder releases any minted handles
    }

    // ---- frame side (consume) ----------------------------------------------
    Frame* frame(xi_frame_handle f) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = frames_.find(f);
        return it == frames_.end() ? nullptr : &it->second.frame;
    }
    void retain(xi_frame_handle f) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = frames_.find(f);
        if (it != frames_.end()) ++it->second.rc;
    }
    void release(xi_frame_handle f) {
        Frame dropped;   // destroy OUTSIDE the lock (releases pool handles)
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = frames_.find(f);
            if (it == frames_.end()) return;
            if (--it->second.rc > 0) return;
            dropped = std::move(it->second.frame);
            frames_.erase(it);
        }
    }

    // Test/diagnostic: how many live frames + builders the table holds. Used by
    // the frame-door tests as a leak oracle alongside ImagePool::cumulative().
    size_t live_frames()   { std::lock_guard<std::mutex> lk(mu_); return frames_.size(); }
    size_t live_builders() { std::lock_guard<std::mutex> lk(mu_); return builders_.size(); }

private:
    struct Slot { Frame frame; int rc = 0; };
    std::mutex mu_;
    std::unordered_map<uint64_t, std::unique_ptr<FrameBuilder>> builders_;
    std::unordered_map<uint64_t, Slot> frames_;
    std::atomic<uint64_t> next_{1};
};

namespace frame_abi_detail {

inline FrameTag tag_from_int(int32_t t) { return static_cast<FrameTag>(t); }

// ---- builder trampolines ---------------------------------------------------
inline xi_frame_builder f_builder_new() { return FrameRegistry::instance().new_builder(); }
inline void f_add_i64(xi_frame_builder b, const char* key, int64_t v) {
    if (auto* fb = FrameRegistry::instance().builder(b)) fb->add_i64(key ? key : "", v);
}
inline void f_add_f64(xi_frame_builder b, const char* key, double v) {
    if (auto* fb = FrameRegistry::instance().builder(b)) fb->add_f64(key ? key : "", v);
}
inline void f_add_str(xi_frame_builder b, const char* key, const char* s, int32_t len) {
    if (auto* fb = FrameRegistry::instance().builder(b))
        fb->add_str(key ? key : "", std::string_view(s ? s : "", len > 0 ? (size_t)len : 0));
}
inline void f_add_bin(xi_frame_builder b, const char* key, const void* data, int32_t len) {
    if (auto* fb = FrameRegistry::instance().builder(b))
        fb->add_bin(key ? key : "", data, len > 0 ? (size_t)len : 0);
}
inline void f_add_image(xi_frame_builder b, const char* key,
                        int32_t w, int32_t h, int32_t c, const void* px) {
    if (auto* fb = FrameRegistry::instance().builder(b)) fb->add_image(key ? key : "", w, h, c, px);
}
inline void f_adopt_image(xi_frame_builder b, const char* key,
                          int32_t w, int32_t h, int32_t c, xi_image_handle handle) {
    if (auto* fb = FrameRegistry::instance().builder(b))
        fb->adopt_image(key ? key : "", w, h, c, handle);
}
inline void f_add_mp(xi_frame_builder b, const char* key, const void* mp, int32_t len) {
    if (auto* fb = FrameRegistry::instance().builder(b))
        fb->add_mp(key ? key : "", mp, len > 0 ? (size_t)len : 0);
}
inline xi_frame_handle f_seal(xi_frame_builder b) { return FrameRegistry::instance().seal(b); }
inline void f_abandon(xi_frame_builder b) { FrameRegistry::instance().abandon(b); }

// ---- accessor trampolines --------------------------------------------------
inline int32_t f_count(xi_frame_handle f) {
    Frame* fr = FrameRegistry::instance().frame(f);
    return fr ? (int32_t)fr->size() : 0;
}
inline const char* f_key_at(xi_frame_handle f, int32_t i, int32_t* len) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || i < 0 || (size_t)i >= fr->size()) { if (len) *len = 0; return nullptr; }
    std::string_view k = fr->key_at((size_t)i);   // borrowed arena bytes, not NUL-terminated
    if (len) *len = (int32_t)k.size();
    return k.data();
}
inline int32_t f_tag_at(xi_frame_handle f, int32_t i) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || i < 0 || (size_t)i >= fr->size()) return -1;
    return (int32_t)fr->tag_at((size_t)i);
}
inline int32_t f_tag_of(xi_frame_handle f, const char* key) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || !key) return -1;
    auto t = fr->tag_of(key);
    return t ? (int32_t)*t : -1;
}
inline int32_t f_get_i64(xi_frame_handle f, const char* key, int64_t* out) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || !key) return 0;
    auto v = fr->get_i64(key);
    if (!v) return 0;
    if (out) *out = *v;
    return 1;
}
inline int32_t f_get_f64(xi_frame_handle f, const char* key, double* out) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || !key) return 0;
    auto v = fr->get_f64(key);
    if (!v) return 0;
    if (out) *out = *v;
    return 1;
}
inline int32_t f_get_str(xi_frame_handle f, const char* key, const char** ptr, int32_t* len) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || !key) return 0;
    auto v = fr->get_str(key);
    if (!v) return 0;
    if (ptr) *ptr = v->data();
    if (len) *len = (int32_t)v->size();
    return 1;
}
inline int32_t f_get_bin(xi_frame_handle f, const char* key, const void** ptr, int32_t* len) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || !key) return 0;
    auto v = fr->get_bin(key);
    if (!v) return 0;
    if (ptr) *ptr = v->data();
    if (len) *len = (int32_t)v->size();
    return 1;
}
inline int32_t f_get_image(xi_frame_handle f, const char* key, xi_frame_image* out) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || !key) return 0;
    auto v = fr->get_image(key);
    if (!v) return 0;
    if (out) {
        out->width    = v->width;
        out->height   = v->height;
        out->channels = v->channels;
        out->pixels   = v->pixels.data();
        out->length   = (int32_t)v->pixels.size();
    }
    return 1;
}
inline int32_t f_get_mp(xi_frame_handle f, const char* key, const void** ptr, int32_t* len) {
    Frame* fr = FrameRegistry::instance().frame(f);
    if (!fr || !key) return 0;
    auto v = fr->get_mp(key);
    if (!v) return 0;
    if (ptr) *ptr = v->data();
    if (len) *len = (int32_t)v->size();
    return 1;
}

// ---- lifetime / emit -------------------------------------------------------
inline void f_retain(xi_frame_handle f)  { FrameRegistry::instance().retain(f); }
inline void f_release(xi_frame_handle f) { FrameRegistry::instance().release(f); }

// The emit_frame forwarder. The caller (an SDK source) still holds its own ref;
// we take a SECOND ref for the async dispatch event and hand it to the bus,
// which consumes it (stores it on the event or releases it if there's no sink).
inline void f_emit_frame(const char* emitter, xi_trigger_id id,
                         xi_frame_handle f, int64_t ts) {
    if (f == XI_FRAME_NULL) return;
    FrameRegistry::instance().retain(f);               // the event's ref
    TriggerBus::instance().emit_frame(emitter ? emitter : "", id, ts, f);
}

// The releaser the bus/dispatcher calls to drop an event's frame ref.
inline void f_release_for_bus(xi_frame_handle f) { FrameRegistry::instance().release(f); }

} // namespace frame_abi_detail

// The process-stable xi.frame@1 data-plane interface. Meyers singleton, so its
// address (and every fn-pointer) is stable for the host's life — a plugin caches
// the pointer once (Plugin::frame_iface()).
inline const xi_frame_v1* frame_v1_iface() {
    static const xi_frame_v1 iface = {
        &frame_abi_detail::f_builder_new,
        &frame_abi_detail::f_add_i64,
        &frame_abi_detail::f_add_f64,
        &frame_abi_detail::f_add_str,
        &frame_abi_detail::f_add_bin,
        &frame_abi_detail::f_add_image,
        &frame_abi_detail::f_adopt_image,
        &frame_abi_detail::f_add_mp,
        &frame_abi_detail::f_seal,
        &frame_abi_detail::f_abandon,
        &frame_abi_detail::f_count,
        &frame_abi_detail::f_key_at,
        &frame_abi_detail::f_tag_at,
        &frame_abi_detail::f_tag_of,
        &frame_abi_detail::f_get_i64,
        &frame_abi_detail::f_get_f64,
        &frame_abi_detail::f_get_str,
        &frame_abi_detail::f_get_bin,
        &frame_abi_detail::f_get_image,
        &frame_abi_detail::f_get_mp,
        &frame_abi_detail::f_retain,
        &frame_abi_detail::f_release,
        &frame_abi_detail::f_emit_frame,
    };
    return &iface;
}

// Publish the Frame data plane so host->get_interface("xi.frame", 1) resolves it,
// and wire the dispatch releaser so a frame event's ref is dropped correctly.
// Idempotent; call once next to install_trigger_hook (default_host_api / certify)
// and in any test that builds a host_api it drives frames through.
inline void install_frame_abi() {
    ImagePool::publish_frame_iface(frame_v1_iface());
    TriggerBus::instance().set_frame_releaser(&frame_abi_detail::f_release_for_bus);
}

} // namespace xi
