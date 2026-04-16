#pragma once
//
// xi_abi.hpp — C++ wrapper over the stable C plugin ABI.
//
// Plugin authors write a class deriving from xi::Plugin, override
// process() and exchange(), then put XI_PLUGIN_IMPL(MyClass) at the
// bottom. The macro generates all 6 C entry points.
//
// Images are handles managed by the host. The wrapper provides a
// HostImage class that acts like xi::Image but backed by a host handle.
// Copying a HostImage = addref (zero-copy). Destroying = release.
//
// Usage:
//
//   class MyPlugin : public xi::Plugin {
//   public:
//       using xi::Plugin::Plugin;  // inherit ctor
//
//       xi::Record process(const xi::Record& input) override {
//           auto img = input.get_image("frame");
//           auto gray = xi::ops::toGray(img);   // works with HostImage
//           return xi::Record()
//               .image("gray", gray)
//               .set("done", true);
//       }
//   };
//
//   XI_PLUGIN_IMPL(MyPlugin)
//

#include "xi_abi.h"
#include "xi_image.hpp"
#include "xi_record.hpp"

#include <cstring>
#include <map>
#include <string>

namespace xi {

// --- HostImage: an Image backed by a host-managed handle ---
//
// HostImage wraps a xi_image_handle. It refcounts via the host API:
// copy → addref, destroy → release. Data access via host->image_data().
//
// HostImage is implicitly convertible to/from xi::Image so existing
// operator code (toGray, threshold, etc.) works unchanged.

class HostImage {
public:
    HostImage() = default;

    HostImage(const xi_host_api* host, xi_image_handle h)
        : host_(host), handle_(h) {
        if (host_ && handle_) host_->image_addref(handle_);
    }

    ~HostImage() {
        if (host_ && handle_) host_->image_release(handle_);
    }

    HostImage(const HostImage& o) : host_(o.host_), handle_(o.handle_) {
        if (host_ && handle_) host_->image_addref(handle_);
    }

    HostImage& operator=(const HostImage& o) {
        if (this != &o) {
            if (host_ && handle_) host_->image_release(handle_);
            host_ = o.host_;
            handle_ = o.handle_;
            if (host_ && handle_) host_->image_addref(handle_);
        }
        return *this;
    }

    HostImage(HostImage&& o) noexcept : host_(o.host_), handle_(o.handle_) {
        o.handle_ = XI_IMAGE_NULL;
    }

    HostImage& operator=(HostImage&& o) noexcept {
        if (this != &o) {
            if (host_ && handle_) host_->image_release(handle_);
            host_ = o.host_;
            handle_ = o.handle_;
            o.handle_ = XI_IMAGE_NULL;
        }
        return *this;
    }

    // Convert to xi::Image (copies pixel data — use when you need to
    // pass to operators that expect xi::Image)
    operator Image() const {
        if (!host_ || !handle_) return {};
        int w = host_->image_width(handle_);
        int h = host_->image_height(handle_);
        int c = host_->image_channels(handle_);
        const uint8_t* p = const_cast<const xi_host_api*>(host_)->image_data(handle_);
        return Image(w, h, c, p);
    }

    // Create a HostImage from an xi::Image (copies pixel data into host pool)
    static HostImage from_image(const xi_host_api* host, const Image& img) {
        if (!host || img.empty()) return {};
        xi_image_handle h = host->image_create(img.width, img.height, img.channels);
        if (!h) return {};
        uint8_t* dst = host->image_data(h);
        std::memcpy(dst, img.data(), img.size());
        HostImage hi;
        hi.host_ = host;
        hi.handle_ = h;  // already refcount=1 from create, don't addref
        return hi;
    }

    bool empty() const { return !handle_ || !host_; }
    xi_image_handle handle() const { return handle_; }
    const xi_host_api* host() const { return host_; }

    uint8_t*       data()     { return host_ ? host_->image_data(handle_) : nullptr; }
    const uint8_t* data() const { return host_ ? host_->image_data(handle_) : nullptr; }
    int width()    const { return host_ ? host_->image_width(handle_) : 0; }
    int height()   const { return host_ ? host_->image_height(handle_) : 0; }
    int channels() const { return host_ ? host_->image_channels(handle_) : 0; }

private:
    const xi_host_api* host_ = nullptr;
    xi_image_handle    handle_ = XI_IMAGE_NULL;
};

// --- Plugin base class ---

class Plugin {
public:
    Plugin(const xi_host_api* host, const std::string& name)
        : host_(host), name_(name) {}

    virtual ~Plugin() = default;

    const xi_host_api* host() const { return host_; }
    const std::string& name() const { return name_; }

    // Override these in your plugin:
    virtual Record process(const Record& input) { (void)input; return {}; }
    virtual std::string exchange(const std::string& cmd_json) { (void)cmd_json; return "{}"; }
    virtual std::string get_def() const { return "{}"; }
    virtual bool set_def(const std::string& json) { (void)json; return true; }

protected:
    // Create a host-managed image (refcount = 1)
    HostImage create_image(int w, int h, int ch) {
        if (!host_) return {};
        xi_image_handle handle = host_->image_create(w, h, ch);
        HostImage hi;
        // Direct construction — handle already has refcount=1
        return HostImage(host_, handle);
    }

    void log_info(const std::string& msg) {
        if (host_ && host_->log) host_->log(1, msg.c_str());
    }
    void log_error(const std::string& msg) {
        if (host_ && host_->log) host_->log(3, msg.c_str());
    }

    const xi_host_api* host_;
    std::string name_;
};

// --- Conversion helpers ---

// Convert a C xi_record to a C++ Record (images become HostImages → copied to xi::Image)
inline Record record_from_c(const xi_host_api* host, const xi_record* rec) {
    Record r;
    if (rec->json) {
        cJSON* parsed = cJSON_Parse(rec->json);
        if (parsed) {
            cJSON* item = parsed->child;
            while (item) {
                if (cJSON_IsNumber(item))      r.set(item->string, item->valuedouble);
                else if (cJSON_IsBool(item))   r.set(item->string, cJSON_IsTrue(item) ? true : false);
                else if (cJSON_IsString(item)) r.set(item->string, std::string(item->valuestring));
                else                           r.set_raw(item->string, cJSON_Duplicate(item, true));
                item = item->next;
            }
            cJSON_Delete(parsed);
        }
    }
    for (int i = 0; i < rec->image_count; ++i) {
        // Convert handle → xi::Image (copies pixels)
        auto& entry = rec->images[i];
        int w = host->image_width(entry.handle);
        int h = host->image_height(entry.handle);
        int ch = host->image_channels(entry.handle);
        const uint8_t* p = host->image_data(entry.handle);
        if (p && w > 0 && h > 0) {
            r.image(entry.key, Image(w, h, ch, p));
        }
    }
    return r;
}

// Convert a C++ Record to a C xi_record_out (images → host handles)
inline void record_to_c(const xi_host_api* host, const Record& r, xi_record_out* out) {
    xi_record_out_init(out);
    std::string json = r.data_json();
    xi_record_out_set_json(out, json.c_str());
    for (auto& [key, img] : r.images()) {
        if (img.empty()) continue;
        xi_image_handle h = host->image_create(img.width, img.height, img.channels);
        if (h) {
            uint8_t* dst = host->image_data(h);
            std::memcpy(dst, img.data(), img.size());
            xi_record_out_add_image(out, key.c_str(), h);
        }
    }
}

} // namespace xi

// --- XI_PLUGIN_IMPL macro ---

#define XI_PLUGIN_IMPL(ClassName)                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
void* xi_plugin_create(const xi_host_api* host, const char* name) {            \
    return new ClassName(host, name);                                           \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
void xi_plugin_destroy(void* inst) {                                           \
    delete static_cast<ClassName*>(inst);                                       \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
void xi_plugin_process(void* inst,                                             \
                       const xi_record* input,                                 \
                       xi_record_out* output) {                                \
    auto* self = static_cast<ClassName*>(inst);                                \
    xi::Record in_rec = xi::record_from_c(self->host(), input);                \
    xi::Record out_rec = self->process(in_rec);                                \
    xi::record_to_c(self->host(), out_rec, output);                            \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_exchange(void* inst, const char* cmd,                            \
                       char* rsp, int rsplen) {                                \
    auto* self = static_cast<ClassName*>(inst);                                \
    std::string r = self->exchange(cmd);                                       \
    int n = (int)r.size();                                                     \
    if (rsplen < n + 1) return -n;                                             \
    std::memcpy(rsp, r.data(), r.size());                                      \
    rsp[r.size()] = 0;                                                         \
    return n;                                                                  \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_get_def(void* inst, char* buf, int buflen) {                     \
    auto* self = static_cast<ClassName*>(inst);                                \
    std::string d = self->get_def();                                           \
    int n = (int)d.size();                                                     \
    if (buflen < n + 1) return -n;                                             \
    std::memcpy(buf, d.data(), d.size());                                      \
    buf[d.size()] = 0;                                                         \
    return n;                                                                  \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_set_def(void* inst, const char* json) {                          \
    return static_cast<ClassName*>(inst)->set_def(json) ? 0 : -1;              \
}
