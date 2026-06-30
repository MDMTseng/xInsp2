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
#include "xi_clock.hpp"
#include "xi_record.hpp"   // wire codec is yyjson JSON (Record::from_json_bytes / data_json)
#include "xi_image.hpp"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Defined in xi_script_support.hpp (force-included by the compiler)
extern void* g_use_process_fn_;
extern void* g_use_exchange_fn_;
extern void* g_use_grab_fn_;
extern void* g_use_host_api_;   // xi_host_api* into BACKEND's ImagePool
extern void* g_trigger_info_fn_;
extern void* g_trigger_image_fn_;
extern void* g_trigger_sources_fn_;
extern void* g_trigger_leader_fn_;
extern void* g_trigger_meta_fn_;

namespace xi {

// Microseconds since the Unix epoch (system_clock). Same clock the host
// uses to stamp TriggerEvent::timestamp_us / dequeued_at_us, so script-
// side subtraction across the host/script boundary is meaningful.
// Defined here (and not pulled from xi_trigger_bus.hpp) so scripts don't
// have to include host-only headers to compute the latency split.
// Guarded so the host's xi_trigger_bus.hpp can also define it without
// ODR conflict when both headers land in the same TU.
#ifndef XI_NOW_US_DEFINED
#define XI_NOW_US_DEFINED
// Thin compat aliases over the canonical clock (xi_clock.hpp). Both now_us and
// steady_now_us are defined together so the guard can't leave one undefined
// depending on header include order.
inline int64_t now_us()        { return xi::wall_us(); }
inline int64_t steady_now_us() { return xi::mono_us(); }
#endif

// Function pointer types for the callbacks
// γ: `input_doc` carries the caller's borrowed yyjson_mut_doc* for the in-process
// zero-serialize path. The host callback uses it directly when the target plugin
// is doc-compatible, else serialises it to JSON itself — so the caller never
// pays data_json() up front. `input_data`/`input_len` stay in the signature but
// are null from the doc path (legacy/explicit-JSON callers may still set them).
using UseProcessFn  = int (*)(const char* name,
                              const void* input_doc,
                              const uint8_t* input_data, int32_t input_len,
                              const xi_record_image* input_images, int input_image_count,
                              xi_record_out* output);
using UseExchangeFn = int (*)(const char* name, const char* cmd,
                              char* rsp, int rsplen);
using UseGrabFn     = xi_image_handle (*)(const char* name, int timeout_ms);

// Trigger callbacks (host-side wires these in via xi_script_set_trigger_callbacks)
struct CurrentTriggerInfo {
    xi_trigger_id id;
    int64_t       timestamp_us;     // emit timestamp (host->emit_trigger ts)
    int32_t       is_active;        // 0 if no trigger is currently being dispatched
    int32_t       _pad = 0;         // align dequeued_at_us to 8 bytes
    int64_t       dequeued_at_us;   // moment dispatcher worker popped this event
                                    // off g_ev_queue (same clock as timestamp_us).
                                    // queue_wait_us = dequeued_at_us - timestamp_us
                                    // inspect_us    = now_us()       - dequeued_at_us
};
using TriggerInfoFn    = void (*)(CurrentTriggerInfo* out);
using TriggerImageFn   = xi_image_handle (*)(const char* source);
using TriggerSourcesFn = int32_t (*)(char* buf, int32_t buflen);
// Returns the policy leader's source name (or empty string when the
// trigger has no leader, e.g. policy=any). Same buf-or-needed convention
// as TriggerSourcesFn: positive return = bytes written, negative return
// = -needed_bytes (caller resizes and retries), 0 = no leader.
using TriggerLeaderFn  = int32_t (*)(char* buf, int32_t buflen);
// ABI v5: returns the event's metadata doc (emit_trigger_record) as a
// yyjson_mut_doc* with one ref RESERVED for the caller to adopt_shared (==
// consume) — exactly the share_out/adopt_shared handshake the process()-input
// doc uses. null when the trigger carries no metadata.
using TriggerMetaFn    = void* (*)();

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

    // Microseconds (system_clock, same base as timestamp_us) when the
    // dispatcher worker pulled this event off the internal queue. Useful
    // for separating "time spent waiting in queue" from "time spent in
    // process()":
    //
    //   double queue_wait_us = (double)(t.dequeued_at_us() - t.timestamp_us());
    //   double inspect_us    = (double)(xi::now_us()        - t.dequeued_at_us());
    //
    // 0 if the host hasn't stamped one (e.g. single-shot cmd:run path
    // before this field was introduced, or synthetic timer ticks with
    // no trigger). Always check is_active() first.
    int64_t       dequeued_at_us() const { ensure(); return info_.dequeued_at_us; }

    std::string id_string() const {
        ensure();
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)info_.id.hi,
                      (unsigned long long)info_.id.lo);
        return buf;
    }

    // Returns the named source's image as a zero-copy view over the
    // host pool handle. `fn` returned a fresh ref (refcount=1);
    // adopt_pool_handle addrefs (=2); we release once (=1) — the
    // remaining ref is owned by the returned Image's shared_ptr deleter.
    // Sources with multiple frames-per-trigger use the key
    // "<source>/<image_name>"; for the common single-frame case the
    // key is just the source name.
    Image image(const std::string& source) const {
        auto fn = reinterpret_cast<TriggerImageFn>(g_trigger_image_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!fn || !host) return {};
        xi_image_handle h = fn(source.c_str());
        if (h == XI_IMAGE_NULL) return {};
        Image img = Image::adopt_pool_handle(host, h);
        host->image_release(h);
        return img;
    }

    // Source names present in this trigger (\n-separated single allocation).
    //
    // Two-pass against the host: first call with a small stack buffer; if
    // the host returns a negative value (= -needed_bytes), retry once with
    // a heap buffer of the requested size. Avoids both an unconditional
    // heap allocation (common case is <100 bytes) and silent truncation
    // for projects with many or long source names.
    std::vector<std::string> sources() const {
        auto fn = reinterpret_cast<TriggerSourcesFn>(g_trigger_sources_fn_);
        if (!fn) return {};
        char stack_buf[512];
        int32_t n = fn(stack_buf, sizeof(stack_buf));
        std::vector<std::string> out;
        std::string s;
        if (n > 0) {
            s.assign(stack_buf, (size_t)n);
        } else if (n < 0) {
            int32_t needed = -n;
            std::vector<char> heap((size_t)needed + 1);
            int32_t n2 = fn(heap.data(), (int32_t)heap.size());
            if (n2 <= 0) return out;
            s.assign(heap.data(), (size_t)n2);
        } else {
            return out;
        }
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find('\n', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) out.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    // Policy leader's source name. For `policy:"leader_followers"` this
    // is the leader instance; for `policy:"any"` it's the source that
    // emitted this event; for `policy:"all_required"` it may be empty.
    // Falls back to the first sources() entry when the host has no
    // leader callback (older backends) or returns empty.
    std::string primary_source() const {
        auto fn = reinterpret_cast<TriggerLeaderFn>(g_trigger_leader_fn_);
        if (fn) {
            char stack_buf[256];
            int32_t n = fn(stack_buf, sizeof(stack_buf));
            if (n > 0) return std::string(stack_buf, (size_t)n);
            if (n < 0) {
                int32_t needed = -n;
                std::vector<char> heap((size_t)needed + 1);
                int32_t n2 = fn(heap.data(), (int32_t)heap.size());
                if (n2 > 0) return std::string(heap.data(), (size_t)n2);
            }
        }
        // Fallback: first source name (matches policy=any semantics).
        auto srcs = sources();
        return srcs.empty() ? std::string{} : srcs.front();
    }

    // ABI v5: the event's routing/context metadata (whatever the source put in
    // the record it emit_trigger_record'd), as a read-only Record. Zero-copy /
    // zero-serialize — borrows the host-owned metadata doc by pointer (held by
    // the DocRegistry refcount for the life of this dispatch), exactly as a
    // plugin's process(in) borrows in.doc. Read fields off it like any Record:
    //
    //   auto t = xi::current_trigger();
    //   if (t.is_active()) {
    //       auto m = t.meta();
    //       std::string cmd = m["command"].as_string();   // routing key
    //   }
    //
    // Returns an empty Record when the trigger carries no metadata (the bare
    // emit_trigger path) or the host predates this callback. The returned
    // Record is FROZEN (the host still holds its own ref): reads are free,
    // a mutation copy-on-writes into the script's own doc.
    Record meta() const {
        auto fn   = reinterpret_cast<TriggerMetaFn>(g_trigger_meta_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!fn || !host || !host->doc_release) return Record();
        void* d = fn();
        if (!d) return Record();
        // adopt_shared CONSUMES the ref trigger_meta_cb reserved for us; the
        // returned Record doc_release's it when it dies, balancing the reserve.
        return Record::adopt_shared((yyjson_mut_doc*)d, host->doc_release, /*frozen=*/true);
    }

    // True if `name` appears in this trigger's source list. Cheap routing
    // affordance for multi-source scripts that switch on source identity
    // without re-implementing string compare or hashing in the hot path.
    bool has_source(const char* name) const {
        if (!name) return false;
        for (auto& s : sources()) if (s == name) return true;
        return false;
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
// A miss on xi::use("name") — process/exchange returns -1 when no instance by
// that name is registered — used to be silent (empty Record/Image, no log), so a
// typo'd or not-yet-created instance name looked like "found nothing". Surface it
// once per name as an error log so it's discoverable.
inline void warn_use_miss_(const xi_host_api* host, const char* name) {
    if (!host || !host->log) return;
    static std::mutex mu;
    static std::unordered_map<std::string, bool> warned;
    std::string key = name ? name : "";
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!warned.emplace(key, true).second) return;   // warned this name already
    }
    std::string msg = "xi::use(\"" + key + "\"): no such instance — process/exchange "
                      "returns empty (typo, or instance not created yet?)";
    host->log(3, msg.c_str());
}

class UseProxy {
public:
    explicit UseProxy(const std::string& name) : name_(name) {}

    Record process(const Record& input) {
        // NA propagation: a poison input short-circuits — the plugin never runs,
        // and the NA (with its reason) flows straight through. See
        // docs/internals/typed-io.md.
        if (input.is_na()) return Record::na(input.na_reason()).set_src(name_);

        auto process_fn = reinterpret_cast<UseProcessFn>(g_use_process_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!process_fn || !host) return {};

        // Marshal input Record → C ABI. Pool-backed Images forward
        // their existing handle (just addref); heap-backed Images
        // allocate a new pool slot and memcpy the bytes in. The
        // receiving plugin sees the handle either way.
        // Per-thread scratch, reused across calls to drop two allocations off
        // the per-frame dispatch path. Safe because use() calls never nest on a
        // thread: process_fn is a plugin, and a plugin can't call xi::use()
        // (it's script-side), so it can't re-enter this with the buffers live.
        static thread_local std::vector<xi_record_image> in_imgs;
        static thread_local std::vector<xi_image_handle>  in_handles;
        in_imgs.clear();
        in_handles.clear();
        for (auto& [key, img] : input.images()) {
            if (img.empty()) continue;
            xi_image_handle h = XI_IMAGE_NULL;
            if (img.pool_handle() && img.pool_host() == host) {
                h = img.pool_handle();
                host->image_addref(h);
            } else {
                h = host->image_create(img.width, img.height, img.channels);
                if (h == XI_IMAGE_NULL) continue;
                std::memcpy(host->image_data(h), img.data(), img.size());
            }
            in_imgs.push_back({key.c_str(), h});
            in_handles.push_back(h);
        }
        xi_record_out output;
        xi_record_out_init(&output);

        // γ-4: share our input doc into the host registry (enroll this side) and
        // hand the host the registry-managed pointer, so the plugin can adopt it
        // and cache it across frames zero-copy — no serialize. For a JSON-fallback
        // target the host serialises it there; the plugin never adopts, and our
        // enroll ref is released when this input Record dies.
        const void* in_doc = (host->doc_retain && host->doc_release)
            ? (const void*)input.share_out(host->doc_retain, host->doc_release)
            : (const void*)input.doc();
        int prc = process_fn(name_.c_str(), in_doc,
                   nullptr, 0,
                   in_imgs.data(), (int)in_imgs.size(), &output);
        // Release input handles from the BACKEND pool — plugin's process()
        // copied what it needed. Done regardless of outcome.
        for (auto h : in_handles) host->image_release(h);

        // A failed call's `output` is NOT a trustworthy result: -1 = no such
        // instance; -2 = the plugin's process() crashed (SEH) or threw — in which
        // case it may have written a partial/torn out_doc before faulting. Adopting
        // or parsing it would be a use-after-fault, and treating a crashed call as a
        // valid (empty) result is a silent false-pass downstream. Bail with an empty
        // provenance-tagged Record instead of interpreting output. (Previously only
        // -1 was special-cased and the crash path fell through to adopt_shared.)
        if (prc < 0) {
            if (prc == -1) {
                warn_use_miss_(host, name_.c_str());
                // The instance was never found, so use_process_inline_ returned at
                // its first line WITHOUT touching the shared doc — the ref share_out
                // RESERVED for the adopting side (xi_record.hpp share_out) is
                // unconsumed. Release it here or it (and the host-owned doc + its
                // pooled chunks) leak on EVERY call — a per-frame unbounded leak when
                // a script use()'s a renamed/typo'd instance under continuous dispatch.
                // Scoped to the share_out path: in_doc is the registry pointer only
                // when doc_retain/doc_release were present (else it's a borrowed
                // input.doc() we must NOT release). The JSON-fallback and adopt paths
                // already balance their ref; -2 (crash) is left alone (a torn call may
                // or may not have adopted — don't risk a double-release).
                if (host->doc_retain && host->doc_release && in_doc)
                    host->doc_release(const_cast<void*>(in_doc));
            }
            xi_record_out_free(&output);
            Record empty;
            empty.set_src(name_);
            return empty;
        }

        // γ: adopt the borrowed-doc output by pointer (zero parse) when the
        // plugin returned one; otherwise decode the JSON bytes. The adopted doc
        // is host-pool-backed, so freeing it when `result` dies routes through
        // the host (doc->alc) — safe across the DLL boundary.
        Record result = output.out_doc
            ? Record::adopt_shared((yyjson_mut_doc*)output.out_doc, host->doc_release,
                                   host->doc_refcount && host->doc_refcount((void*)output.out_doc) > 1)
            : ((output.data && output.len > 0)
                   ? Record::from_json_bytes(output.data, (size_t)output.len)
                   : Record());
        // Output handles live in the BACKEND pool. Zero-copy: wrap as
        // a pool-backed view (adopt_pool_handle addrefs internally) and
        // release our process_fn ref. Net refcount: still 1, held by
        // the script-side xi::Image via its shared_ptr deleter.
        for (int i = 0; i < output.image_count; ++i) {
            xi_image_handle h = output.images[i].handle;
            if (!h) continue;
            Image img = Image::adopt_pool_handle(host, h);
            host->image_release(h);
            if (!img.empty()) result.image(output.images[i].key, std::move(img));
        }
        xi_record_out_free(&output);
        result.set_src(name_);   // provenance: this output came from this instance
        return result;
    }

    std::string exchange(const std::string& cmd) {
        auto exchange_fn = reinterpret_cast<UseExchangeFn>(g_use_exchange_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!exchange_fn) return "{}";
        // Per-thread scratch, reused (and kept at its high-water size) instead of
        // a fresh 64KB allocation every call. Same non-nesting safety as
        // process(): exchange_fn is a plugin and can't re-enter xi::use().
        static thread_local std::vector<char> buf;
        if (buf.size() < 64 * 1024) buf.resize(64 * 1024);
        int n = exchange_fn(name_.c_str(), cmd.c_str(), buf.data(), (int)buf.size());
        if (n == -1) { warn_use_miss_(host, name_.c_str()); return "{}"; }  // no such instance
        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                     n = exchange_fn(name_.c_str(), cmd.c_str(), buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    Image grab(int timeout_ms = 500) {
        auto grab_fn = reinterpret_cast<UseGrabFn>(g_use_grab_fn_);
        auto* host   = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!grab_fn || !host) return {};
        xi_image_handle h = grab_fn(name_.c_str(), timeout_ms);
        if (h == XI_IMAGE_NULL) return {};
        Image img = Image::adopt_pool_handle(host, h);
        host->image_release(h);
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
