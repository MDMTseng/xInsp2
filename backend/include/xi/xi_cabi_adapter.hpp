#pragma once
//
// xi_cabi_adapter.hpp — the primitives for loading and wrapping a SINGLE
// C-ABI plugin: the ABI-version gate (plugin_abi_compatible), the loaded-plugin
// descriptor (PluginInfo: manifest fields + DLL handle + factory pointers), and
// the adapter (CAbiInstanceAdapter) that presents a C-ABI plugin instance as an
// xi::InstanceBase. Extracted from xi_plugin_manager.hpp so the per-plugin
// loading layer is separable from PluginManager (discovery / project lifecycle).
//
// No PluginManager / project-model dependency here — strictly the DLL + instance
// wrapping layer.
//

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include "xi_abi.h"
#include "xi_image_pool.hpp"
#include "xi_instance.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace xi {

// Plugin ABI compatibility check. Reads the plugin DLL's
// xi_plugin_abi_version() export and compares against the host's
// XI_ABI_VERSION. Pre-versioning plugins (no export) are accepted as
// v1 with a one-shot warning logged; plugins requesting a newer ABI
// than the host provides are refused (caller should FreeLibrary +
// skip + record a warning).
inline bool plugin_abi_compatible(HMODULE dll, const std::string& plugin_name,
                                   std::string* err_msg = nullptr) {
    using AbiVerFn = int (*)();
    auto fn = reinterpret_cast<AbiVerFn>(GetProcAddress(dll, "xi_plugin_abi_version"));
    int v = fn ? fn() : 1;
    if (v > XI_ABI_VERSION) {
        if (err_msg) *err_msg = "plugin '" + plugin_name + "' requires ABI v"
                              + std::to_string(v) + " but host is v"
                              + std::to_string(XI_ABI_VERSION);
        return false;
    }
    if (!fn) {
        std::fprintf(stderr,
            "[xinsp2] '%s': pre-versioning plugin (no xi_plugin_abi_version "
            "export); assuming v1\n", plugin_name.c_str());
    }
    return true;
}

struct PluginInfo {
    std::string name;
    std::string description;
    std::string dll_name;
    std::string factory_symbol;
    bool        has_ui = false;
    // `reentrant`: the plugin declares its process()/exchange()/get_def()/
    // set_def() are safe to call CONCURRENTLY on the same instance. When false
    // (the default) the host serializes calls per instance with a mutex, so a
    // parallel dispatch pool (parallelism.dispatch_threads > 1) is safe by
    // default — only plugins that opt in get true per-instance parallelism.
    // See docs/guides/writing-a-script.md (parallelism) + plugin-abi.md.
    bool        reentrant = false;
    std::string folder_path;   // absolute path to plugin folder
    std::string ui_path;       // absolute path to ui/ folder (if has_ui)
    HMODULE     handle = nullptr;

    // Optional. If `plugin.json` has a top-level `manifest` object, its
    // raw JSON text lands here. The backend doesn't validate or reshape
    // it — clients (AI agents, doc tools) parse the content themselves.
    // Convention (free-form): `params` / `inputs` / `outputs` / `exchange`
    // arrays describing what the plugin tunes, consumes, produces, and
    // accepts via exchange_instance. See docs/reference/plugin-abi.md.
    std::string manifest_json;

    // Old-style factory: InstanceBase* (name)
    using FactoryFn = InstanceBase* (*)(const char* instance_name);
    FactoryFn factory = nullptr;
    // New C ABI factory: void* (host_api, name)
    using CFactoryFn = void* (*)(const xi_host_api* host, const char* name);
    CFactoryFn c_factory = nullptr;
};

// Adapter: wraps a C ABI plugin instance as an InstanceBase
class CAbiInstanceAdapter : public InstanceBase {
public:
    CAbiInstanceAdapter(std::string name, std::string plugin_name,
                        HMODULE dll, void* inst, bool reentrant = false,
                        int max_concurrency = 0)
        : name_(std::move(name)), plugin_name_(std::move(plugin_name)),
          dll_(dll), inst_(inst), reentrant_(reentrant),
          owner_id_(ImagePool::alloc_owner_id()) {
        max_concurrency_ = (max_concurrency > 0 ? max_concurrency : 0);
        // Resolve function pointers
        exchange_fn_ = reinterpret_cast<xi_plugin_exchange_fn>(GetProcAddress(dll_, "xi_plugin_exchange"));
        get_def_fn_  = reinterpret_cast<xi_plugin_get_def_fn>(GetProcAddress(dll_, "xi_plugin_get_def"));
        set_def_fn_  = reinterpret_cast<xi_plugin_set_def_fn>(GetProcAddress(dll_, "xi_plugin_set_def"));
        destroy_fn_  = reinterpret_cast<xi_plugin_destroy_fn>(GetProcAddress(dll_, "xi_plugin_destroy"));
        process_fn_  = reinterpret_cast<xi_plugin_process_fn>(GetProcAddress(dll_, "xi_plugin_process"));
    }

    ~CAbiInstanceAdapter() override {
        if (destroy_fn_ && inst_) destroy_fn_(inst_);
        // Sweep any image handles the plugin allocated and forgot to
        // release. Without this, plugin crashes / careless authors leak
        // ImagePool entries forever.
        int swept = ImagePool::instance().release_all_for(owner_id_);
        if (swept > 0) {
            std::fprintf(stderr,
                "[xinsp2] '%s' destroyed; swept %d leaked image handle(s)\n",
                name_.c_str(), swept);
        }
    }

    ImagePoolOwnerId owner_id() const { return owner_id_; }
    // Replace the auto-allocated id with one the caller pre-allocated
    // (used by open_project / create_instance to tag handles the
    // plugin's ctor allocates BEFORE the adapter exists). The
    // sweep-on-destroy still sees the right bucket either way.
    void adopt_owner_id(ImagePoolOwnerId id) { owner_id_ = id; }

    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return plugin_name_; }

    // OwnerGuard wraps every plugin entry-point call so any image
    // handles the plugin allocates via host_api->image_create get
    // tagged with this instance's owner_id. The destructor's
    // release_all_for then knows what to sweep.
    std::string get_def() const override {
        if (!get_def_fn_ || !inst_) return "{}";
        ImagePool::OwnerGuard g(owner_id_);
        CallScope cs(this);
        std::vector<char> buf(4096);
        int n = get_def_fn_(inst_, buf.data(), (int)buf.size());
        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024); n = get_def_fn_(inst_, buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    bool set_def(const std::string& j) override {
        if (!set_def_fn_ || !inst_) return false;
        ImagePool::OwnerGuard g(owner_id_);
        CallScope cs(this);
        return set_def_fn_(inst_, j.c_str()) == 0;
    }

    std::string exchange(const std::string& cmd_json) override {
        if (!exchange_fn_ || !inst_) return "{}";
        ImagePool::OwnerGuard g(owner_id_);
        CallScope cs(this);
        std::vector<char> buf(64 * 1024);
        int n = exchange_fn_(inst_, cmd_json.c_str(), buf.data(), (int)buf.size());
        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024); n = exchange_fn_(inst_, cmd_json.c_str(), buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    // Run the plugin's process() entry point. Wraps the OwnerGuard (image-leak
    // tagging) and, for a non-reentrant plugin, the per-instance lock so a
    // parallel dispatch pool can't re-enter the same instance's state
    // concurrently. Returns output->image_count, or -1 if no process fn.
    // The caller owns the SEH try/catch boundary (use_process_cb).
    int process(const xi_record* in, xi_record_out* out) {
        if (!process_fn_ || !inst_) return -1;
        ImagePool::OwnerGuard og(owner_id_);
        CallScope cs(this);
        process_fn_(inst_, in, out);
        return out->image_count;
    }

    void* raw_instance() const { return inst_; }
    xi_plugin_process_fn process_fn() const { return process_fn_; }
    bool reentrant() const { return reentrant_; }

private:
    // Effective concurrency cap across process/exchange/get_def/set_def:
    //   non-reentrant      -> 1   (serialized; safety — ignores max_concurrency_)
    //   reentrant + cap    -> max_concurrency_ (>=1)
    //   reentrant + no cap -> 0   (unlimited)
    int effective_cap_() const { return reentrant_ ? max_concurrency_ : 1; }

    // RAII admission control: blocks until fewer than `cap` calls are in flight
    // on this instance, counts itself in, releases on scope exit. cap==0
    // (unlimited) is a no-op. One counter across all entry points so a config
    // change (exchange/set_def) can't race an in-flight process(). Replaces the
    // old binary mutex — count-1 reproduces the non-reentrant serialization.
    struct CallScope {
        const CAbiInstanceAdapter* a_;
        bool engaged_;
        explicit CallScope(const CAbiInstanceAdapter* a) : a_(a) {
            int cap = a_->effective_cap_();
            engaged_ = (cap != 0);
            if (!engaged_) return;
            std::unique_lock<std::mutex> lk(a_->cc_mu_);
            a_->cc_cv_.wait(lk, [&] { return a_->cur_calls_ < cap; });
            ++a_->cur_calls_;
        }
        ~CallScope() {
            if (!engaged_) return;
            { std::lock_guard<std::mutex> lk(a_->cc_mu_); --a_->cur_calls_; }
            a_->cc_cv_.notify_one();
        }
        CallScope(const CallScope&) = delete;
        CallScope& operator=(const CallScope&) = delete;
    };

    mutable std::mutex              cc_mu_;
    mutable std::condition_variable cc_cv_;
    mutable int                     cur_calls_ = 0;
    bool reentrant_ = false;
    int  max_concurrency_ = 0;     // 0 = unlimited (reentrant only)
    std::string name_;
    std::string plugin_name_;
    HMODULE dll_;
    void* inst_;
    xi_plugin_exchange_fn exchange_fn_ = nullptr;
    xi_plugin_get_def_fn  get_def_fn_ = nullptr;
    xi_plugin_set_def_fn  set_def_fn_ = nullptr;
    xi_plugin_destroy_fn  destroy_fn_ = nullptr;
    xi_plugin_process_fn  process_fn_ = nullptr;
    ImagePoolOwnerId      owner_id_ = 0;
};

} // namespace xi
