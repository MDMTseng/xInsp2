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

namespace xi {

// Function pointer types for the callbacks
using UseProcessFn  = int (*)(const char* name,
                              const char* input_json,
                              const xi_record_image* input_images, int input_image_count,
                              xi_record_out* output);
using UseExchangeFn = int (*)(const char* name, const char* cmd,
                              char* rsp, int rsplen);
using UseGrabFn     = xi_image_handle (*)(const char* name, int timeout_ms);

// Proxy object returned by xi::use()
class UseProxy {
public:
    explicit UseProxy(const std::string& name) : name_(name) {}

    Record process(const Record& input) {
        auto process_fn = reinterpret_cast<UseProcessFn>(g_use_process_fn_);
        if (!process_fn) return {};

        // Marshal input Record → C ABI
        std::vector<xi_record_image> in_imgs;
        std::vector<xi_image_handle> in_handles;
        for (auto& [key, img] : input.images()) {
            xi_image_handle h = ImagePool::instance().from_image(img);
            in_handles.push_back(h);
            in_imgs.push_back({key.c_str(), h});
        }
        std::string json = input.data_json();

        xi_record_out output;
        xi_record_out_init(&output);

        process_fn(name_.c_str(), json.c_str(),
                   in_imgs.data(), (int)in_imgs.size(), &output);

        // Release input handles
        for (auto h : in_handles) ImagePool::instance().release(h);

        // Unmarshal output
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
        for (int i = 0; i < output.image_count; ++i) {
            result.image(output.images[i].key,
                         ImagePool::instance().to_image(output.images[i].handle));
            ImagePool::instance().release(output.images[i].handle);
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
