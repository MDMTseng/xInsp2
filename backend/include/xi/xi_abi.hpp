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
#include <vector>

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

    // NOTE: no public (host, handle) constructor. It was a refcount trap:
    // `host->image_create()` returns a handle with refcount=1, so
    // `HostImage(host, host->image_create(...))` would leave refcount=2
    // and leak. Use the two named ctors below:
    //   - from_handle(host, h)  — take ownership of an existing handle
    //   - from_image(host, img) — copy an xi::Image into the host pool
    // For the rare case you genuinely want to share (addref) an existing
    // handle, call `share_handle` explicitly.

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
        return from_handle(host, h);  // take existing refcount=1, no addref
    }

    // Take ownership of an existing handle WITHOUT addref (handle already refcount=1)
    static HostImage from_handle(const xi_host_api* host, xi_image_handle h) {
        HostImage hi;
        hi.host_ = host;
        hi.handle_ = h;  // no addref — we take the existing refcount
        return hi;
    }

    // Share an existing handle by addref-ing — for when some other
    // component owns the handle and you want an independently-managed view.
    static HostImage share_handle(const xi_host_api* host, xi_image_handle h) {
        if (host && h) host->image_addref(h);
        return from_handle(host, h);
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

    // On-disk folder for THIS instance: project/instances/<name>/
    // Already created by the host before this plugin was constructed.
    // Use it to persist files beyond the JSON config returned by get_def.
    // Returns empty string if running detached from a project.
    std::string folder_path() const {
        if (!host_ || !host_->instance_folder) return "";
        char buf[1024];
        int32_t n = host_->instance_folder(name_.c_str(), buf, sizeof(buf));
        if (n > 0) return std::string(buf, (size_t)n);
        if (n < 0) {
            std::vector<char> big((size_t)(-n) + 1);
            n = host_->instance_folder(name_.c_str(), big.data(), (int32_t)big.size());
            if (n > 0) return std::string(big.data(), (size_t)n);
        }
        return "";
    }

    // Override these in your plugin:
    virtual Record process(const Record& input) { (void)input; return {}; }
    virtual std::string exchange(const std::string& cmd_json) { (void)cmd_json; return "{}"; }
    virtual std::string get_def() const { return "{}"; }
    virtual bool set_def(const std::string& json) { (void)json; return true; }

protected:
    HostImage create_image(int w, int h, int ch) {
        if (!host_) return {};
        xi_image_handle handle = host_->image_create(w, h, ch);
        // from_image_handle: takes ownership of the existing refcount=1
        // without calling addref again
        return HostImage::from_handle(host_, handle);
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

namespace detail {
// Per-plugin-DLL thread-local storage for the strings populated
// during process_fn. The backend reads `out->images[i].key` and
// `out->json` directly; previously these were `_strdup`/`malloc`'d
// inside the plugin DLL and freed by the backend, which is UB across
// CRT boundaries. Owning them in thread_local std::string +
// std::vector inside the plugin DLL means the same allocator that
// allocated them frees them (when the next process_fn call clears
// the storage, or when the plugin DLL unloads at process exit). The
// strings stay valid until the next call to `process_fn` on the same
// thread — the backend's read happens before that.
struct PluginOutputStorage {
    std::vector<std::string>     keys;
    std::string                  json;
    std::vector<xi_record_image> images;
};
inline PluginOutputStorage& tls_output_storage() {
    static thread_local PluginOutputStorage s;
    return s;
}
} // namespace detail

// Convert a C++ Record to a C xi_record_out (images → host handles).
//
// Strings (image keys + json) live in thread-local storage owned by
// the plugin DLL — see PluginOutputStorage. The output's
// `image_capacity` is set to 0 to signal "no malloc'd backing"; the
// C inline `xi_record_out_free` honours that and skips the free path
// entirely. This closes the cross-CRT heap-corruption hole that
// existed when plugin DLLs and the backend EXE used different CRTs.
inline void record_to_c(const xi_host_api* host, const Record& r, xi_record_out* out) {
    auto& s = detail::tls_output_storage();
    s.keys.clear();
    s.images.clear();
    s.json = r.data_json();

    out->json = const_cast<char*>(s.json.c_str());

    const size_t n = r.images().size();
    s.keys.reserve(n);
    s.images.reserve(n);
    for (auto& [key, img] : r.images()) {
        if (img.empty()) continue;
        xi_image_handle h = host->image_create(img.width, img.height, img.channels);
        if (!h) continue;
        std::memcpy(host->image_data(h), img.data(), img.size());
        s.keys.push_back(key);
        xi_record_image rec{};
        rec.key    = s.keys.back().c_str();
        rec.handle = h;
        s.images.push_back(rec);
    }

    out->images         = s.images.empty() ? nullptr : s.images.data();
    out->image_count    = (int32_t)s.images.size();
    out->image_capacity = 0;   // tls-owned, see xi_record_out_free
}

} // namespace xi

// --- XI_PLUGIN_IMPL macro ---

#define XI_PLUGIN_IMPL(ClassName)                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
void* xi_plugin_create(const xi_host_api* host, const char* name) {            \
    try { return new ClassName(host, name); }                                   \
    catch (...) { return nullptr; }                                             \
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
