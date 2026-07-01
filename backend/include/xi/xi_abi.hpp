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
//           auto src  = input.get_image("frame");
//           auto gray = xi::Image::create_in_pool(host(), src.width, src.height, 1);
//           cv::cvtColor(xi::as_cv_mat(src), xi::as_cv_mat(gray), cv::COLOR_RGB2GRAY);  // needs <xi/xi_cv.hpp>
//           return xi::Record().image("gray", gray).set("done", true);
//       }
//   };
//
//   XI_PLUGIN_IMPL(MyPlugin)
//

#include "xi_abi.h"
#include "xi_image.hpp"
#include "xi_record.hpp"   // wire codec is yyjson JSON (Record::from_json_bytes / data_json)

#include <cstdio>
#include <cstring>
#include <exception>
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

// Forward-decl of the free emit verb (defined below) so Plugin::emit() — the
// member convenience that fills host()/name() for a source — can forward to it.
inline void emit_record(const xi_host_api* host, const char* emitter, Record& r,
                        xi_trigger_id id, int64_t ts);

class Plugin {
public:
    Plugin(const xi_host_api* host, const std::string& name)
        : host_(host), name_(name) {}

    virtual ~Plugin() = default;

    const xi_host_api* host() const { return host_; }
    const std::string& name() const { return name_; }

    // ABI v8: push an opaque binary frame straight to connected WS clients. The
    // host is a dumb byte pipe — the frame FORMAT is this plugin's contract with
    // its UI (it must self-describe: tag/group/key + dims + codec + payload).
    // Safe from a dispatch worker thread. No-op on a pre-v8 host (null emit_binary).
    //
    // ABI v10 capability segregation (Phase 3): resolves the frozen `xi.emit@1`
    // interface via host->get_interface ONCE and caches it; falls back to the
    // legacy host->emit_binary field on a pre-v10 host. Both reach the identical
    // host byte pipe.
    void emit_binary(const void* data, int len) const {
        if (!(data && len > 0)) return;
        if (const xi_emit_v1* ev = emit_iface()) {
            if (ev->emit_binary) { ev->emit_binary(data, (int32_t)len); return; }
        }
        if (host_ && host_->emit_binary)
            host_->emit_binary(data, (int32_t)len);
    }
    void emit_binary(const std::vector<uint8_t>& frame) const {
        emit_binary(frame.data(), (int)frame.size());
    }

    // Emit a Record as a trigger event — the member sibling of the free
    // xi::emit_record(host(), name().c_str(), rec, ...). Fills host_/name_
    // itself so a source can just `emit(rec)` instead of re-passing the
    // emitter it already is. Forwards verbatim to the free fn (same staging,
    // same id/ts defaults: id auto-minted, ts = host now).
    void emit(Record& r, xi_trigger_id id = XI_TRIGGER_NULL, int64_t ts = 0) {
        xi::emit_record(host_, name_.c_str(), r, id, ts);
    }

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
            std::vector<char> big((size_t)(-(int64_t)n) + 1);
            n = host_->instance_folder(name_.c_str(), big.data(), (int32_t)big.size());
            if (n > 0) return std::string(big.data(), (size_t)n);
        }
        return "";
    }

    // Publish this instance's latest status string (host keeps it last-value
    // and serves it via cmd:status). No-op on an older host. Keep it short and
    // human: status("grabbing"), status("model loaded, 3 ROIs").
    //
    // ABI v10 capability segregation (Phase 3): set_status now lives in the frozen
    // `xi.log@1` interface; resolve-then-cache with a legacy-field fallback.
    void status(const std::string& text) const {
        if (const xi_log_v1* lv = log_iface()) {
            if (lv->set_status) { lv->set_status(name_.c_str(), text.c_str()); return; }
        }
        if (host_ && host_->set_status) host_->set_status(name_.c_str(), text.c_str());
    }

    // JPEG-encode an image through the host's content-addressed cache (the same
    // image encoded by several plugins, or repeatedly, is encoded ONCE globally).
    // Returns bytes written into `out`, or -needed if `out_cap` is too small
    // (resize + retry), 0 on error / no encoder available.
    //
    // ABI v10 capability segregation (core_fix_plan.md §12 Phase 2): resolves the
    // frozen `xi.preview@1` interface via host->get_interface ONCE and caches it;
    // falls back to the legacy host->compress_image field when the host is pre-v10
    // (no get_interface) or doesn't publish xi.preview. New plugins ride the
    // segregated interface; old plugins and pre-v10 hosts keep working via the
    // field — neither path is privileged, both hit the identical host encoder.
    int compress(const void* px, int w, int h, int ch, int quality,
                 void* out, int out_cap) const {
        if (const xi_preview_v1* pv = preview_iface())
            return pv->compress(px, w, h, ch, quality, out, out_cap);
        if (host_ && host_->compress_image)
            return host_->compress_image(px, w, h, ch, quality, out, out_cap);
        return 0;
    }

    // --- Phase 3 capability wrappers: xi.imaging@1 / xi.doc@1 ----------------
    // Each resolves its frozen interface ONCE via host->get_interface (cached),
    // and falls back to the legacy xi_host_api field on a pre-v10 host. Both
    // paths reach the identical host primitive — neither is privileged.

    // Decode an image file (PNG/JPEG/BMP/...) into a fresh pool handle (refcount
    // 1; caller releases). XI_IMAGE_NULL on failure / no decoder. xi.imaging@1.
    xi_image_handle read_image_file(const char* path) const {
        if (const xi_imaging_v1* iv = imaging_iface()) {
            if (iv->read_image_file) return iv->read_image_file(path);
        }
        if (host_ && host_->read_image_file) return host_->read_image_file(path);
        return XI_IMAGE_NULL;
    }

    // Host-owned doc chunk allocator (xi.doc@1) — backs a yyjson_mut_doc that is
    // safe to hand across the DLL boundary (its free routes back to the host).
    void*   doc_chunk_alloc(size_t n) const {
        if (const xi_doc_v1* dv = doc_iface()) { if (dv->doc_chunk_alloc) return dv->doc_chunk_alloc(n); }
        return host_ && host_->doc_chunk_alloc ? host_->doc_chunk_alloc(n) : nullptr;
    }
    void*   doc_chunk_realloc(void* p, size_t n) const {
        if (const xi_doc_v1* dv = doc_iface()) { if (dv->doc_chunk_realloc) return dv->doc_chunk_realloc(p, n); }
        return host_ && host_->doc_chunk_realloc ? host_->doc_chunk_realloc(p, n) : nullptr;
    }
    void    doc_chunk_free(void* p) const {
        if (const xi_doc_v1* dv = doc_iface()) { if (dv->doc_chunk_free) { dv->doc_chunk_free(p); return; } }
        if (host_ && host_->doc_chunk_free) host_->doc_chunk_free(p);
    }
    // Host-side doc refcount (xi.doc@1) — the doc analogue of image_addref/release.
    void    doc_retain(void* doc) const {
        if (const xi_doc_v1* dv = doc_iface()) { if (dv->doc_retain) { dv->doc_retain(doc); return; } }
        if (host_ && host_->doc_retain) host_->doc_retain(doc);
    }
    void    doc_release(void* doc) const {
        if (const xi_doc_v1* dv = doc_iface()) { if (dv->doc_release) { dv->doc_release(doc); return; } }
        if (host_ && host_->doc_release) host_->doc_release(doc);
    }
    int32_t doc_refcount(void* doc) const {
        if (const xi_doc_v1* dv = doc_iface()) { if (dv->doc_refcount) return dv->doc_refcount(doc); }
        return host_ && host_->doc_refcount ? host_->doc_refcount(doc) : 0;
    }

    // Override these in your plugin:
    virtual Record process(const Record& input) { (void)input; return {}; }
    virtual std::string exchange(const std::string& cmd_json) { (void)cmd_json; return "{}"; }

    // Helper for the `else` branch of an exchange() if/else-if chain. Returns
    // a JSON object documenting that the command name was not recognised:
    //
    //     {"error":"unknown_command","command":"<name>"}
    //
    // Caller passes the command name they parsed (often via xi_json). This
    // gives drivers a uniform error shape rather than the silent no-op /
    // empty-object that plugin authors otherwise return on fallthrough.
    // P2-4: Surface unknown commands instead of silently dropping them.
    static std::string exchange_unknown_command(const std::string& cmd_name) {
        std::string out = "{\"error\":\"unknown_command\",\"command\":";
        // Inline JSON escape — same minimal handling as everywhere else
        // in xi_abi.hpp; cmd_name is plugin-controlled and short.
        out.push_back('"');
        for (char c : cmd_name) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)(unsigned char)c);
                        out += b;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out += "\"}";
        return out;
    }
    virtual std::string get_def() const { return "{}"; }
    virtual bool set_def(const std::string& json) { (void)json; return true; }

    // Frame-perfect config swap (ABI v7). Heavy-resource plugins override these
    // AND opt in with XI_PLUGIN_STAGED(Class) so the host exports them:
    //   prepare() — load the new config's heavy assets into a BACKGROUND staging
    //     slot. The host calls this UNGATED (concurrent with process()), so the
    //     load never stalls the pipeline — therefore prepare MUST touch ONLY the
    //     staging slot, never live state read by process(). `folder` is this
    //     instance's resource folder (load big assets from there).
    //   commit() — atomically swap staging → live (a cheap pointer swap). The host
    //     calls this gated / after draining dispatch, so it's uncontended.
    // Defaults: prepare ≡ set_def (immediate, host-gated), commit ≡ no-op — a
    // simple plugin overrides neither and the host falls back automatically.
    virtual bool prepare(const std::string& def, const std::string& folder) {
        (void)folder; return set_def(def);
    }
    virtual void commit() {}

protected:
    HostImage create_image(int w, int h, int ch) {
        if (!host_) return {};
        // Phase 3: prefer the frozen xi.imaging@1 interface, fall back to the
        // legacy image_create field on a pre-v10 host (identical pool create).
        xi_image_handle handle = XI_IMAGE_NULL;
        if (const xi_imaging_v1* iv = imaging_iface())
            handle = iv->image_create((int32_t)w, (int32_t)h, (int32_t)ch);
        else if (host_->image_create)
            handle = host_->image_create((int32_t)w, (int32_t)h, (int32_t)ch);
        // from_image_handle: takes ownership of the existing refcount=1
        // without calling addref again
        return HostImage::from_handle(host_, handle);
    }

    // Allocate a fresh pool slot and return it as a refcounted xi::Image
    // view. Bytes written via the Image's `data()` (or via a cv::Mat from
    // the opt-in `xi::as_cv_mat()`) land directly in pool memory — no
    // heap-to-pool copy when this Image is returned from process(). This is
    // the standard way for plugins to produce an output image:
    //
    //   auto dst = pool_image(src.width, src.height, 1);
    //   cv::GaussianBlur(xi::as_cv_mat(src), xi::as_cv_mat(dst), {0,0}, 2.0);  // <xi/xi_cv.hpp>
    //   return xi::Record().image("blurred", dst);
    Image pool_image(int w, int h, int ch) {
        return Image::create_in_pool(host_, w, h, ch);
    }

    // Phase 3: log lives in the frozen xi.log@1 interface; resolve-then-cache
    // with a legacy-field fallback (both reach the host log + operator channel).
    void log_at(int32_t level, const std::string& msg) const {
        if (const xi_log_v1* lv = log_iface()) {
            if (lv->log) { lv->log(level, msg.c_str()); return; }
        }
        if (host_ && host_->log) host_->log(level, msg.c_str());
    }
    void log_debug(const std::string& msg) { log_at(0, msg); }
    void log_info(const std::string& msg)  { log_at(1, msg); }
    void log_warn(const std::string& msg)  { log_at(2, msg); }
    void log_error(const std::string& msg) { log_at(3, msg); }

    const xi_host_api* host_;
    std::string name_;

private:
    // Resolve-once cache for the xi.preview@1 interface (used by compress()).
    // get_interface returns a process-stable, host-owned pointer, so caching it
    // once per instance is safe. `resolved_` distinguishes "not yet looked up"
    // from "looked up, absent" so a pre-v10 host is probed at most once.
    const xi_preview_v1* preview_iface() const {
        if (!preview_resolved_) {
            preview_resolved_ = true;
            if (host_ && host_->get_interface)
                preview_ = static_cast<const xi_preview_v1*>(
                    host_->get_interface("xi.preview", 1));
        }
        return preview_;
    }
    mutable bool                 preview_resolved_ = false;
    mutable const xi_preview_v1* preview_          = nullptr;

    // Phase 3 resolve-once caches for the carved domains — same shape as
    // preview_iface(): probe host->get_interface at most once per instance, then
    // cache the process-stable, host-owned pointer (or nullptr on a pre-v10 host,
    // distinguished from "not yet looked up" by the paired `_resolved_` flag).
    const xi_imaging_v1* imaging_iface() const {
        if (!imaging_resolved_) {
            imaging_resolved_ = true;
            if (host_ && host_->get_interface)
                imaging_ = static_cast<const xi_imaging_v1*>(
                    host_->get_interface("xi.imaging", 1));
        }
        return imaging_;
    }
    const xi_doc_v1* doc_iface() const {
        if (!doc_resolved_) {
            doc_resolved_ = true;
            if (host_ && host_->get_interface)
                doc_ = static_cast<const xi_doc_v1*>(
                    host_->get_interface("xi.doc", 1));
        }
        return doc_;
    }
    const xi_emit_v1* emit_iface() const {
        if (!emit_resolved_) {
            emit_resolved_ = true;
            if (host_ && host_->get_interface)
                emit_ = static_cast<const xi_emit_v1*>(
                    host_->get_interface("xi.emit", 1));
        }
        return emit_;
    }
    const xi_log_v1* log_iface() const {
        if (!log_resolved_) {
            log_resolved_ = true;
            if (host_ && host_->get_interface)
                log_ = static_cast<const xi_log_v1*>(
                    host_->get_interface("xi.log", 1));
        }
        return log_;
    }
    mutable bool                 imaging_resolved_ = false;
    mutable const xi_imaging_v1* imaging_          = nullptr;
    mutable bool                 doc_resolved_     = false;
    mutable const xi_doc_v1*     doc_              = nullptr;
    mutable bool                 emit_resolved_    = false;
    mutable const xi_emit_v1*    emit_             = nullptr;
    mutable bool                 log_resolved_     = false;
    mutable const xi_log_v1*     log_              = nullptr;
};

// --- γ: host doc allocator bridge ---
//
// Adapt the host's doc_chunk_* (size)/(ptr,size)/(ptr) functions to yyjson's
// (ctx, ...) allocator signatures, with ctx = the host_api pointer. A
// yyjson_mut_doc built through this allocator is backed by the host doc pool, so
// its chunks free back to the host (doc->alc.free) and the doc is safe to hand
// across the ABI and free from either side. yyjson copies the alc into the doc,
// so a transient yyjson_alc is fine.
namespace detail {
inline void* doc_alc_malloc(void* ctx, size_t s) {
    return reinterpret_cast<const xi_host_api*>(ctx)->doc_chunk_alloc(s);
}
inline void* doc_alc_realloc(void* ctx, void* p, size_t /*old*/, size_t s) {
    return reinterpret_cast<const xi_host_api*>(ctx)->doc_chunk_realloc(p, s);
}
inline void doc_alc_free(void* ctx, void* p) {
    reinterpret_cast<const xi_host_api*>(ctx)->doc_chunk_free(p);
}
} // namespace detail

inline yyjson_alc make_host_doc_alc(const xi_host_api* host) {
    yyjson_alc a;
    a.malloc  = detail::doc_alc_malloc;
    a.realloc = detail::doc_alc_realloc;
    a.free    = detail::doc_alc_free;
    a.ctx     = const_cast<xi_host_api*>(host);
    return a;
}

// RAII: install the host doc allocator as the thread-local Record allocator for
// the duration of an in-process plugin call, so the docs the plugin builds (its
// output + temporaries) come from the host pool — host-owned, poolable (γ-5),
// safe to hand back. No-op on a pre-v3 host (doc_chunk_alloc null) → default
// allocator, JSON output path. Restores the previous alc on scope exit.
struct HostDocAlcScope {
    yyjson_alc         alc_;
    const yyjson_alc*  prev_;
    explicit HostDocAlcScope(const xi_host_api* host)
        : alc_(host ? make_host_doc_alc(host) : yyjson_alc{}), prev_(tls_doc_alc()) {
        if (host && host->doc_chunk_alloc) tls_doc_alc() = &alc_;
    }
    ~HostDocAlcScope() { tls_doc_alc() = prev_; }
    HostDocAlcScope(const HostDocAlcScope&) = delete;
    HostDocAlcScope& operator=(const HostDocAlcScope&) = delete;
};

// --- Conversion helpers ---

// Convert a C xi_record to a C++ Record (images become HostImages → copied to xi::Image)
inline Record record_from_c(const xi_host_api* host, const xi_record* rec) {
    // γ in-process fast path: when the host handed us a borrowed yyjson doc
    // (same process + matching yyjson layout, gated host-side), read it as a
    // view — no JSON parse, no copy. The view is read-only; the plugin's first
    // mutation copy-on-writes into its own doc, leaving the caller's untouched.
    // Otherwise decode the JSON bytes exactly as before.
    // γ-4: input doc is a registry-managed SHARED doc (the host share_out'd it and
    // enrolled it). We adopt with our own ref so the plugin may cache it across
    // frames zero-copy; frozen=true since the host still holds its side during the
    // call, so the plugin's first mutation copy-on-writes. No doc ⇒ JSON / empty.
    Record r = (rec->doc && host && host->doc_release)
                 ? Record::adopt_shared((yyjson_mut_doc*)rec->doc,
                                        host->doc_release, true)
                 : ((rec->data && rec->len > 0)
                        ? Record::from_json_bytes(rec->data, (size_t)rec->len)
                        : Record());
    for (int i = 0; i < rec->image_count; ++i) {
        // Zero-copy: wrap the handle as a refcounted view over pool
        // memory. The xi::Image addrefs the handle on adopt and releases
        // when its last copy goes away — so the plugin can read pool
        // bytes directly without a memcpy. (The caller of process_fn
        // still owns its own ref on each input handle and releases it
        // independently after the call returns; see UseProxy::process.)
        auto& entry = rec->images[i];
        if (!entry.handle) continue;
        Image img = Image::adopt_pool_handle(host, entry.handle);
        if (!img.empty()) r.image(entry.key, std::move(img));
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
    std::vector<uint8_t>         bytes;   // yyjson JSON bytes (fallback when the doc-pointer fast path isn't taken)
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
inline void record_to_c(const xi_host_api* host, Record& r, xi_record_out* out,
                        bool want_doc = false) {
    auto& s = detail::tls_output_storage();
    s.keys.clear();
    s.images.clear();
    // γ symmetric fast path: when the call came in as a borrowed doc (want_doc)
    // and the host owns a doc allocator, hand the OUTPUT doc back by pointer —
    // no serialize. The output Record was built under HostDocAlcScope, so its
    // doc is host-pool-backed; release the ref to the caller, who adopts and
    // frees it via the host (doc->alc). Otherwise serialize to JSON as before.
    // γ/γ-4 symmetric fast path: when the call arrived as a borrowed doc and the
    // host owns a doc registry, hand the OUTPUT doc back by pointer (zero
    // serialize) as a SHARED, host-refcounted doc — one uniform path, exactly
    // mirroring image_addref. The caller adopt_shared's it, the plugin keeps any
    // cached ref, and the doc dies with the last side. share_out returns null
    // only when there's no owned doc to share (a borrowed input returned as-is);
    // then, and on a pre-v4 host, we serialize to JSON as before.
    yyjson_mut_doc* shared = nullptr;
    if (want_doc && host && host->doc_chunk_alloc &&
        host->doc_retain && host->doc_release &&
        (shared = r.share_out(host->doc_retain, host->doc_release)) != nullptr) {
        out->out_doc = shared;
        out->data = nullptr;
        out->len  = 0;
    } else {
        std::string js = r.data_json();   // yyjson serialize (borrowed view / pre-v4 host)
        s.bytes.assign(js.begin(), js.end());
        out->data = s.bytes.data();
        out->len  = (int32_t)s.bytes.size();
        out->out_doc = nullptr;
    }

    const size_t n = r.images().size();
    s.keys.reserve(n);
    s.images.reserve(n);
    for (auto& [key, img] : r.images()) {
        if (img.empty()) continue;
        xi_image_handle h = XI_IMAGE_NULL;
        if (img.pool_handle() && img.pool_host() == host) {
            // Zero-copy forward: this Image is already a view over a
            // pool handle from THIS host. Hand the same handle out (with
            // a fresh ref) instead of allocating a new slot and memcpy'ing
            // pixels we already have in the pool.
            h = img.pool_handle();
            host->image_addref(h);
        } else {
            // Fresh / heap-backed Image — allocate a pool slot and copy
            // the bytes in. (One copy on the way out per genuinely-new
            // image is structurally unavoidable.)
            h = host->image_create(img.width, img.height, img.channels);
            if (!h) continue;
            std::memcpy(host->image_data(h), img.data(), img.size());
        }
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

// Emit a Record as a trigger event WITH routing/context metadata.
//
// The ONE emit verb (added in ABI v6; current XI_ABI_VERSION is higher — see
// xi_abi.h): a source hands the host a record (images +
// metadata) under an id; the host stages it and dispatches one inspection. The
// script reads it back via xi::current_trigger().image()/.meta()/.id_string().
// The metadata doc is handed over by pointer (zero-serialize) through the same
// share_out/adopt refcount handshake the process() path uses — no JSON round
// trip on the live path.
//
//   auto rec = xi::Record()
//       .image("frame", img)
//       .set("command", "inspect_top")     // ← routing/context metadata
//       .set("recipe", 7);
//   xi::emit_record(host(), name().c_str(), rec);   // id auto-minted, ts = now
//
// id == XI_TRIGGER_NULL asks the host to mint a fresh id (its hex is
// id_string()). ts = 0 stamps the host's current time. No-op on a host without
// emit_record (the slot is null).
inline void emit_record(const xi_host_api* host, const char* emitter, Record& r,
                        xi_trigger_id id = XI_TRIGGER_NULL,
                        int64_t ts = 0) {
    if (!host) return;
    // Marshal images → host pool handles (same forward logic as record_to_c).
    // Locals, not TLS: emit is synchronous — the bus addref's the handles and
    // consumes the doc ref during the call, so we can release our refs after.
    std::vector<std::string>     keys;
    std::vector<xi_record_image> entries;
    std::vector<xi_image_handle> mine;     // refs we own, released post-emit
    keys.reserve(r.images().size());       // reserve so key c_str()s don't move
    entries.reserve(r.images().size());
    for (auto& [key, img] : r.images()) {
        if (img.empty()) continue;
        xi_image_handle h = XI_IMAGE_NULL;
        if (img.pool_handle() && img.pool_host() == host) {
            h = img.pool_handle();
            host->image_addref(h);
        } else {
            h = host->image_create(img.width, img.height, img.channels);
            if (!h) continue;
            std::memcpy(host->image_data(h), img.data(), img.size());
        }
        mine.push_back(h);
        keys.push_back(key);
        xi_record_image e{};
        e.key = keys.back().c_str();
        e.handle = h;
        entries.push_back(e);
    }

    if (host->emit_record) {
        // Hand the metadata doc over by pointer: share_out reserves a ref the
        // host consumes. Null when there's no owned doc (nothing to carry).
        yyjson_mut_doc* shared = (host->doc_retain && host->doc_release)
            ? r.share_out(host->doc_retain, host->doc_release) : nullptr;
        xi_record rec{};
        rec.images      = entries.empty() ? nullptr : entries.data();
        rec.image_count = (int32_t)entries.size();
        rec.data        = nullptr;
        rec.len         = 0;
        rec.doc         = shared;
        host->emit_record(emitter, id, &rec, ts);
    }
    for (auto h : mine) host->image_release(h);
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
    /* A destructor throwing across the C ABI is UB — swallow it in-plugin. */  \
    try { delete static_cast<ClassName*>(inst); }                               \
    catch (...) { std::fprintf(stderr, "[xinsp2] plugin destructor threw\n"); } \
}                                                                              \
                                                                               \
/* B2 defense-in-depth: every per-call export catches C++ exceptions HERE, in   */ \
/* the plugin's OWN runtime. A throw that escaped across the extern "C" boundary */ \
/* into the host unwinds correctly only for a source-mode plugin (same CRT as   */ \
/* the host); a PREBUILT plugin built against a different MSVC runtime unwinding */ \
/* across the DLL boundary is UB. Catching in-plugin makes the boundary noexcept */ \
/* in practice: the throw is caught in the same runtime that raised it, and the  */ \
/* host sees a safe sentinel instead of a corrupt unwind. Catch bodies allocate  */ \
/* nothing (a bad_alloc catch must not re-throw).                                */ \
extern "C" __declspec(dllexport)                                               \
void xi_plugin_process(void* inst,                                             \
                       const xi_record* input,                                 \
                       xi_record_out* output) {                                \
    auto* self = static_cast<ClassName*>(inst);                                \
    try {                                                                      \
        /* γ: build the input view / output doc under the host doc allocator,  */ \
        /* return by doc-pointer when the input arrived as one (symmetric).    */ \
        xi::HostDocAlcScope _xi_alc(self->host());                             \
        xi::Record in_rec = xi::record_from_c(self->host(), input);            \
        xi::Record out_rec = self->process(in_rec);                            \
        xi::record_to_c(self->host(), out_rec, output, input->doc != nullptr); \
    } catch (const std::exception& e) {                                        \
        std::fprintf(stderr, "[xinsp2] plugin process() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin process() threw (non-std)\n");   \
    } /* on throw: output stays host-initialised (empty) — no crash */         \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_exchange(void* inst, const char* cmd,                            \
                       char* rsp, int rsplen) {                                \
    auto* self = static_cast<ClassName*>(inst);                                \
    try {                                                                      \
        std::string r = self->exchange(cmd);                                   \
        int n = (int)r.size();                                                 \
        if (rsplen < n + 1) return -n;                                         \
        std::memcpy(rsp, r.data(), r.size());                                  \
        rsp[r.size()] = 0;                                                     \
        return n;                                                              \
    } catch (const std::exception& e) {                                        \
        std::fprintf(stderr, "[xinsp2] plugin exchange() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin exchange() threw (non-std)\n");  \
    }                                                                          \
    return 0; /* safe sentinel: empty response, host won't re-grow the buf */  \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_get_def(void* inst, char* buf, int buflen) {                     \
    auto* self = static_cast<ClassName*>(inst);                                \
    try {                                                                      \
        std::string d = self->get_def();                                       \
        int n = (int)d.size();                                                 \
        if (buflen < n + 1) return -n;                                         \
        std::memcpy(buf, d.data(), d.size());                                  \
        buf[d.size()] = 0;                                                     \
        return n;                                                              \
    } catch (const std::exception& e) {                                        \
        std::fprintf(stderr, "[xinsp2] plugin get_def() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin get_def() threw (non-std)\n");   \
    }                                                                          \
    return 0; /* safe sentinel: empty def */                                   \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_set_def(void* inst, const char* json) {                          \
    try { return static_cast<ClassName*>(inst)->set_def(json) ? 0 : -1; }      \
    catch (const std::exception& e) {                                          \
        std::fprintf(stderr, "[xinsp2] plugin set_def() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin set_def() threw (non-std)\n");   \
    }                                                                          \
    return -1; /* sentinel: config rejected */                                 \
}                                                                              \
                                                                               \
/* ABI version stamp — host loader checks this against its own        */ \
/* XI_ABI_VERSION and refuses plugins requesting a newer ABI than     */ \
/* the host provides. Plugins compiled before this stamp existed are  */ \
/* treated as v1 with a "pre-versioning" warning, not refused.        */ \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_abi_version(void) {                                              \
    return XI_ABI_VERSION;                                                     \
}                                                                              \
                                                                               \
/* yyjson layout stamp (ABI v3, γ). The host hands this plugin a raw          */ \
/* yyjson_mut_doc* (in-process zero-serialize) ONLY if this stamp matches     */ \
/* the host's own — so a prebuilt plugin carrying a different yyjson version  */ \
/* /layout transparently falls back to the JSON data/len path instead of      */ \
/* dereferencing an incompatible struct. Mixes the yyjson version with the    */ \
/* two struct sizes the doc-pointer path depends on. */                        \
extern "C" __declspec(dllexport)                                               \
uint32_t xi_yyjson_abi(void) {                                                 \
    return xi::yyjson_layout_stamp();                                          \
}

// Opt into the ABI v7 frame-perfect config swap. Place AFTER XI_PLUGIN_IMPL.
// ONLY use this if your plugin OVERRIDES prepare()/commit() with a real double-
// slot — the host calls prepare UNGATED (concurrent with process), and the
// CONTRACT is that prepare touches the staging slot ONLY, never live state. A
// plugin that doesn't export these (didn't use this macro) is driven through the
// host's gated set_def fallback instead, which is always safe.
#define XI_PLUGIN_STAGED(ClassName)                                            \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_prepare(void* inst, const char* def_json, const char* folder) {  \
    try {                                                                      \
        return static_cast<ClassName*>(inst)->prepare(def_json ? def_json : "", \
                                                  folder ? folder : "") ? 0 : -1; \
    } catch (const std::exception& e) {                                        \
        std::fprintf(stderr, "[xinsp2] plugin prepare() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin prepare() threw (non-std)\n");   \
    }                                                                          \
    return -1; /* sentinel: staging failed -> host keeps the live config */    \
}                                                                              \
extern "C" __declspec(dllexport)                                               \
void xi_plugin_commit(void* inst) {                                            \
    try { static_cast<ClassName*>(inst)->commit(); }                           \
    catch (const std::exception& e) {                                          \
        std::fprintf(stderr, "[xinsp2] plugin commit() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin commit() threw (non-std)\n");    \
    }                                                                          \
}
