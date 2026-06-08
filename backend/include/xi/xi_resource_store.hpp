#pragma once
//
// xi_resource_store.hpp — host-provided per-emitter resource ring backing the
// emit/fetch dispatch model (see docs/design/emitter-fetch-model).
//
// An emitter STAGES a frame — named images + cJSON metadata — under an opaque
// string res_id via emit(); a consumer later FETCHES it by res_id via fetch(). The
// store is the "ring building block" the design hands emitters so each one
// doesn't reimplement buffering: it keeps a bounded ring of the most recent
// res_ids per emitter name and evicts the oldest past capacity.
//
// Image-handle lifetime (same discipline as the trigger bus): emit() ADDREFS
// each handle it retains (callers may release immediately after). Eviction,
// overwrite, and clear() RELEASE them. fetch() ADDREFS handles handed to the
// caller (caller releases). So the store never frees a handle out from under a
// live consumer, and never leaks one.
//
// Identity vs order: res_id is opaque (may be a UUID) — used only for addressing
// and replay. Ordering for downstream serialization (e.g. PLC sends) rides as a
// `seq` field INSIDE the cJSON metadata, assigned by the emitter; the store
// neither reads nor needs it.
//
#include "xi_abi.h"
#include "xi_image_pool.hpp"

#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xi {

class ResourceStore {
public:
    static ResourceStore& instance() { static ResourceStore s; return s; }

    using ImageList = std::vector<std::pair<std::string, xi_image_handle>>;

    // How many recent res_ids to retain per emitter name (default 16). The ring
    // is the emitter's recent-frame buffer; hot-param re-run reads the latest.
    void set_capacity(size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        cap_ = n ? n : 1;
    }

    // Stage a frame under res_id: copy key strings, addref handles, keep cJSON.
    // Overwrites an existing res_id (releasing its old handles); evicts the
    // oldest once the per-name ring exceeds capacity.
    void emit(const std::string& name, const std::string& res_id,
              const xi_record_image* images, int image_count,
              const std::string& cjson) {
        Entry e;
        e.res_id = res_id;
        e.cjson  = cjson;
        for (int i = 0; i < image_count; ++i) {
            ImagePool::instance().addref(images[i].handle);
            e.images.emplace_back(images[i].key ? images[i].key : "", images[i].handle);
        }
        std::lock_guard<std::mutex> lk(mu_);
        auto& ring = rings_[name];
        for (auto& slot : ring) {                 // overwrite same res_id in place
            if (slot.res_id == res_id) { release_(slot); slot = std::move(e); return; }
        }
        ring.push_back(std::move(e));
        while (ring.size() > cap_) { release_(ring.front()); ring.pop_front(); }
    }

    // Existence + metadata only (no image addref). Backs host fetch_resource.
    bool fetch_meta(const std::string& name, const std::string& res_id,
                  std::string& out_cjson) {
        std::lock_guard<std::mutex> lk(mu_);
        auto rit = rings_.find(name);
        if (rit == rings_.end()) return false;
        for (auto& e : rit->second)
            if (e.res_id == res_id) { out_cjson = e.cjson; return true; }
        return false;
    }

    // Fetch one image by key (addref'd; caller releases). NULL if absent. Backs
    // host fetch_image — lazy single-image fetch.
    xi_image_handle fetch_image(const std::string& name, const std::string& res_id,
                              const std::string& key) {
        std::lock_guard<std::mutex> lk(mu_);
        auto rit = rings_.find(name);
        if (rit == rings_.end()) return XI_IMAGE_NULL;
        for (auto& e : rit->second) {
            if (e.res_id != res_id) continue;
            for (auto& im : e.images)
                if (im.first == key) {
                    ImagePool::instance().addref(im.second);
                    return im.second;
                }
            return XI_IMAGE_NULL;
        }
        return XI_IMAGE_NULL;
    }

    // Release everything (e.g. on project close / shutdown).
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [name, ring] : rings_)
            for (auto& e : ring) release_(e);
        rings_.clear();
    }

private:
    struct Entry {
        std::string res_id;
        std::string cjson;
        ImageList   images;
    };
    void release_(Entry& e) {
        for (auto& im : e.images) ImagePool::instance().release(im.second);
        e.images.clear();
    }
    std::mutex mu_;
    size_t     cap_ = 16;
    std::unordered_map<std::string, std::deque<Entry>> rings_;
};

// Dispatch sink: emit_dispatch routes through this so the lane-routing logic
// (group resolution + enqueue), which lives in the backend's service_main, stays
// out of this leaf header — mirrors how TriggerBus's sink is wired. service_main
// installs it via set_dispatch_sink; until then emit_dispatch is a no-op (e.g.
// the headless runner that has no lane pool). Returns true if the run was
// accepted, false on back-pressure (lane full / not running).
using DispatchSink = std::function<bool(const std::string& emitter,
                                        xi_trigger_id res_id, int64_t timestamp_us)>;
inline DispatchSink& dispatch_sink_() { static DispatchSink s; return s; }
inline void set_dispatch_sink(DispatchSink s) { dispatch_sink_() = std::move(s); }

// Wire the resource-store host hooks into a host_api table. Called alongside
// install_trigger_hook wherever a plugin/script host_api is built.
inline void install_resource_hooks(xi_host_api& api) {
    api.emit_resource = [](const char* emitter_name, const char* res_id,
                           const xi_record_image* images, int32_t image_count,
                           const char* cjson) {
        if (!emitter_name || !res_id) return;
        ResourceStore::instance().emit(emitter_name, res_id, images,
                                       image_count < 0 ? 0 : image_count,
                                       cjson ? cjson : "");
    };
    api.fetch_resource = [](const char* emitter_name, const char* res_id,
                          char* cjson_buf, int32_t cjson_buflen) -> int32_t {
        if (!emitter_name || !res_id) return -1;
        std::string cj;
        if (!ResourceStore::instance().fetch_meta(emitter_name, res_id, cj)) return -1;
        int32_t L = (int32_t)cj.size();
        if (cjson_buf && L <= cjson_buflen) std::memcpy(cjson_buf, cj.data(), (size_t)L);
        return L;   // L > buflen: caller resizes to L and retries
    };
    api.fetch_image = [](const char* emitter_name, const char* res_id,
                                const char* key) -> xi_image_handle {
        if (!emitter_name || !res_id) return XI_IMAGE_NULL;
        return ResourceStore::instance().fetch_image(emitter_name, res_id, key ? key : "");
    };
    api.emit_dispatch = [](const char* emitter_name, xi_trigger_id res_id,
                           int64_t timestamp_us) -> int32_t {
        if (!emitter_name) return 0;
        auto& sink = dispatch_sink_();
        return (sink && sink(emitter_name, res_id, timestamp_us)) ? 1 : 0;
    };
}

} // namespace xi
