#pragma once
//
// xi_use.hpp — script-side proxy to access backend-managed instances.
//
// Scripts call xi::use("cam0") to get a proxy object that routes
// process() and exchange() calls back to the backend's InstanceRegistry
// via C ABI thunks. The backend owns the instance — scripts never
// create or destroy them.
//
// Usage:
//
//   void xi_inspect_entry(int frame) {
//       auto& cam = xi::use("cam0");
//       auto& det = xi::use("det0");
//       auto img = cam.grab();
//       auto result = det.process(xi::Record().image("gray", img));
//   }
//

#include "xi_abi.h"
#include "xi_record.hpp"
#include "xi_image.hpp"
#include "xi_image_pool.hpp"

#include <cstring>
#include <string>
#include <unordered_map>

// Defined in xi_script_support.hpp (force-included by the compiler)
extern void* g_use_process_fn_;
extern void* g_use_exchange_fn_;
extern void* g_use_grab_fn_;
extern void* g_use_host_api_;   // xi_host_api* into BACKEND's ImagePool
extern void* g_trigger_info_fn_;
extern void* g_trigger_image_fn_;
extern void* g_trigger_sources_fn_;

namespace xi {

// Function pointer types for the callbacks
using UseProcessFn  = int (*)(const char* name,
                              const char* input_json,
                              const xi_record_image* input_images, int input_image_count,
                              xi_record_out* output);
using UseExchangeFn = int (*)(const char* name, const char* cmd,
                              char* rsp, int rsplen);
using UseGrabFn     = xi_image_handle (*)(const char* name, int timeout_ms);

// Trigger callbacks (host-side wires these in via xi_script_set_trigger_callbacks)
struct CurrentTriggerInfo {
    xi_trigger_id id;
    int64_t       timestamp_us;
    int32_t       is_active;       // 0 if no trigger is currently being dispatched
};
using TriggerInfoFn    = void (*)(CurrentTriggerInfo* out);
using TriggerImageFn   = xi_image_handle (*)(const char* source);
using TriggerSourcesFn = int32_t (*)(char* buf, int32_t buflen);

// xi::Trigger — read-only view of the current inspection event.
//
//   void xi_inspect_entry(int frame) {
//       auto t = xi::current_trigger();
//       if (t.is_active()) {
//           auto img = t.image("cam_left");          // correlated frames
//           auto right = t.image("cam_right");
//           VAR(tid, t.id_string());
//       }
//   }
//
class Trigger {
public:
    Trigger() = default;

    bool is_active() const {
        auto info_fn = reinterpret_cast<TriggerInfoFn>(g_trigger_info_fn_);
        if (!info_fn) return false;
        CurrentTriggerInfo info{};
        info_fn(&info);
        if (!info.is_active) return false;
        info_ = info;
        loaded_ = true;
        return true;
    }

    xi_trigger_id id() const          { ensure(); return info_.id; }
    int64_t       timestamp_us() const { ensure(); return info_.timestamp_us; }

    std::string id_string() const {
        ensure();
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)info_.id.hi,
                      (unsigned long long)info_.id.lo);
        return buf;
    }

    // Returns the named source's image, copied into a script-side Image.
    // Sources with multiple frames-per-trigger use the key
    // "<source>/<image_name>"; for the common single-frame case the
    // key is just the source name.
    Image image(const std::string& source) const {
        auto fn = reinterpret_cast<TriggerImageFn>(g_trigger_image_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!fn || !host) return {};
        xi_image_handle h = fn(source.c_str());
        if (h == XI_IMAGE_NULL) return {};
        int w = host->image_width(h);
        int hh = host->image_height(h);
        int ch = host->image_channels(h);
        const uint8_t* p = host->image_data(h);
        Image img;
        if (p && w > 0 && hh > 0) img = Image(w, hh, ch, p);
        host->image_release(h);
        return img;
    }

    // Source names present in this trigger (\n-separated single allocation).
    std::vector<std::string> sources() const {
        auto fn = reinterpret_cast<TriggerSourcesFn>(g_trigger_sources_fn_);
        if (!fn) return {};
        char buf[2048];
        int32_t n = fn(buf, sizeof(buf));
        std::vector<std::string> out;
        if (n <= 0) return out;
        std::string s(buf, (size_t)n);
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find('\n', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) out.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

private:
    void ensure() const {
        if (loaded_) return;
        auto info_fn = reinterpret_cast<TriggerInfoFn>(g_trigger_info_fn_);
        if (!info_fn) return;
        info_fn(&info_);
        loaded_ = true;
    }
    mutable CurrentTriggerInfo info_{};
    mutable bool               loaded_ = false;
};

// Per-call helper. Cheap to construct — internal info is fetched lazily.
inline Trigger current_trigger() { return Trigger{}; }

// Proxy object returned by xi::use()
class UseProxy {
public:
    explicit UseProxy(const std::string& name) : name_(name) {}

    Record process(const Record& input) {
        auto process_fn = reinterpret_cast<UseProcessFn>(g_use_process_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!process_fn || !host) return {};

        // Marshal input Record → C ABI. Allocate handles in the BACKEND
        // pool (via host_api) so the receiving plugin can resolve them.
        std::vector<xi_record_image> in_imgs;
        std::vector<xi_image_handle>  in_handles;
        for (auto& [key, img] : input.images()) {
            if (img.empty()) continue;
            xi_image_handle h = host->image_create(img.width, img.height, img.channels);
            if (h == XI_IMAGE_NULL) continue;
            std::memcpy(host->image_data(h), img.data(), img.size());
            in_imgs.push_back({key.c_str(), h});
            in_handles.push_back(h);
        }
        std::string json = input.data_json();

        xi_record_out output;
        xi_record_out_init(&output);

        process_fn(name_.c_str(), json.c_str(),
                   in_imgs.data(), (int)in_imgs.size(), &output);

        // Release input handles from the BACKEND pool — plugin's process()
        // copied what it needed.
        for (auto h : in_handles) host->image_release(h);

        Record result;
        if (output.json) {
            cJSON* parsed = cJSON_Parse(output.json);
            if (parsed) {
                cJSON* item = parsed->child;
                while (item) {
                    if (cJSON_IsNumber(item))      result.set(item->string, item->valuedouble);
                    else if (cJSON_IsBool(item))   result.set(item->string, cJSON_IsTrue(item) ? true : false);
                    else if (cJSON_IsString(item)) result.set(item->string, std::string(item->valuestring));
                    else                           result.set_raw(item->string, cJSON_Duplicate(item, true));
                    item = item->next;
                }
                cJSON_Delete(parsed);
            }
        }
        // Output handles live in the BACKEND pool. Copy pixels into a
        // script-side Image, then release the backend handle.
        for (int i = 0; i < output.image_count; ++i) {
            xi_image_handle h = output.images[i].handle;
            int w  = host->image_width(h);
            int hi = host->image_height(h);
            int ch = host->image_channels(h);
            const uint8_t* p = host->image_data(h);
            if (p && w > 0 && hi > 0) {
                result.image(output.images[i].key, Image(w, hi, ch, p));
            }
            host->image_release(h);
        }
        xi_record_out_free(&output);
        return result;
    }

    std::string exchange(const std::string& cmd) {
        auto exchange_fn = reinterpret_cast<UseExchangeFn>(g_use_exchange_fn_);
        if (!exchange_fn) return "{}";
        std::vector<char> buf(64 * 1024);
        int n = exchange_fn(name_.c_str(), cmd.c_str(), buf.data(), (int)buf.size());
        if (n < 0) { buf.resize((size_t)(-n) + 1024);
                     n = exchange_fn(name_.c_str(), cmd.c_str(), buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    Image grab(int timeout_ms = 500) {
        auto grab_fn = reinterpret_cast<UseGrabFn>(g_use_grab_fn_);
        if (!grab_fn) return {};
        xi_image_handle h = grab_fn(name_.c_str(), timeout_ms);
        if (h == XI_IMAGE_NULL) return {};
        Image img = ImagePool::instance().to_image(h);
        ImagePool::instance().release(h);
        return img;
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
};

// Thread-safe cache of proxies — one per name, lazily created
inline UseProxy& use(const std::string& name) {
    static std::unordered_map<std::string, UseProxy> proxies;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    auto it = proxies.find(name);
    if (it == proxies.end()) {
        it = proxies.emplace(name, UseProxy(name)).first;
    }
    return it->second;
}

} // namespace xi
