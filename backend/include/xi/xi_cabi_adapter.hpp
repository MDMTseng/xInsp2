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
#include "xi_record.hpp"   // γ: yyjson_layout_stamp() for the doc-pointer gate

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace xi {

// Plugin ABI compatibility check. Two gates, run at load (caller FreeLibrary +
// skip + record the warning on a false return):
//   1. ABI VERSION — reads xi_plugin_abi_version(); a plugin requesting a newer
//      ABI than the host provides is refused. Pre-versioning plugins (no export)
//      are treated as v1.
//   2. yyjson LAYOUT (γ-4) — reads xi_yyjson_abi(); if it doesn't match the
//      host's stamp (different yyjson build/version) or is absent, the plugin can
//      only run the slow JSON-serialize path on every dispatch. We REFUSE it by
//      default so that perf cliff is visible, unless the manifest opted in with
//      `"json_fallback": true` (json_fallback_opt_in) — then it loads on the JSON
//      path with a one-shot warning.
inline bool plugin_abi_compatible(HMODULE dll, const std::string& plugin_name,
                                  bool json_fallback_opt_in,
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
    // γ-4 yyjson layout gate.
    auto yfn = reinterpret_cast<uint32_t(*)()>(GetProcAddress(dll, "xi_yyjson_abi"));
    bool layout_ok = yfn && (yfn() == xi::yyjson_layout_stamp());
    if (!layout_ok) {
        if (!json_fallback_opt_in) {
            if (err_msg) *err_msg = "plugin '" + plugin_name + "' has an incompatible "
                "yyjson layout (" + (yfn ? "different yyjson build/version"
                                         : "no xi_yyjson_abi export")
                + ") — it can only run the slow JSON-serialize path, not the "
                  "zero-copy doc path. Rebuild it against the host's vendored "
                  "yyjson, or set \"json_fallback\": true in its plugin.json to "
                  "allow the JSON path.";
            return false;
        }
        std::fprintf(stderr,
            "[xinsp2] '%s': yyjson layout mismatch — running on JSON fallback "
            "(json_fallback opt-in; serializes every dispatch)\n",
            plugin_name.c_str());
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
    // See docs/guides/write-a-script.md (parallelism) + plugin-abi.md.
    bool        reentrant = false;
    // Opt-in (plugin.json `"json_fallback": true`): allow this plugin to load
    // even when its yyjson layout doesn't match the host's — it then runs the
    // slow JSON-serialize path on every dispatch instead of the zero-copy doc
    // path. Without it, a layout mismatch (or a plugin with no xi_yyjson_abi
    // export) is REFUSED at load so the perf cliff is never silent. See
    // plugin_abi_compatible / docs/reference/c-abi.md.
    bool        json_fallback = false;
    // Build mode (plugin.json `"build"`): `"source"` (default) = the backend
    // compiles the plugin's .cpp with cl.exe in-place (PluginDev flags). `"cmake"`
    // (alias `"prebuilt": true`) = the plugin owns its build (its own CMakeLists
    // for external libs / CUDA); the backend never invokes cl.exe on it and just
    // loads the prebuilt `build/<name>.dll`. `xInsp2: Rebuild Plugins` runs cmake
    // on cmake-mode plugins, then hot-reloads whichever DLLs actually changed.
    // See docs/guides/write-a-plugin.md (External libraries & CUDA).
    bool        prebuilt = false;
    std::string folder_path;   // absolute path to plugin folder
    std::string ui_path;       // absolute path to ui/ folder (if has_ui)
    HMODULE     handle = nullptr;
    // Last-loaded DLL stamp (write-time ticks + size) — the change-gate for
    // reload_changed: a rebuild only triggers a hot-swap when these move.
    uint64_t    loaded_dll_mtime = 0;
    uint64_t    loaded_dll_size  = 0;

    // Optional. If `plugin.json` has a top-level `manifest` object, its
    // raw JSON text lands here. The backend doesn't validate or reshape
    // it — clients (AI agents, doc tools) parse the content themselves.
    // Convention (free-form): `params` / `inputs` / `outputs` / `exchange`
    // arrays describing what the plugin tunes, consumes, produces, and
    // accepts via exchange_instance. See docs/reference/c-abi.md.
    std::string manifest_json;

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
        // ABI v7 (optional — present only if the plugin opted in with
        // XI_PLUGIN_STAGED). Their presence IS the opt-in signal: a plugin that
        // exports prepare promises it touches only its staging slot, so we may
        // call it ungated (concurrent with process). Absent → gated set_def / no-op.
        prepare_fn_  = reinterpret_cast<xi_plugin_prepare_fn>(GetProcAddress(dll_, "xi_plugin_prepare"));
        commit_fn_   = reinterpret_cast<xi_plugin_commit_fn>(GetProcAddress(dll_, "xi_plugin_commit"));
        // γ: may we hand this plugin a borrowed yyjson_mut_doc* (in-process zero-
        // serialize input)? Only if it was built against our yyjson layout.
        if (auto abi_fn = reinterpret_cast<uint32_t(*)()>(GetProcAddress(dll_, "xi_yyjson_abi")))
            doc_input_ok_ = (abi_fn() == xi::yyjson_layout_stamp());
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

    // Frame-perfect config swap (ABI v7). prepare loads the new config's heavy
    // assets into the plugin's background staging slot; it runs UNGATED — NO
    // CallScope — so it proceeds concurrent with process() and never stalls the
    // pipeline. That is sound ONLY because a plugin that exports xi_plugin_prepare
    // (via XI_PLUGIN_STAGED) contracts to touch staging ONLY. A plugin without
    // the export falls back to the base prepare ≡ set_def, which IS gated (safe).
    bool prepare(const std::string& def, const std::string& folder) override {
        if (!prepare_fn_ || !inst_) return InstanceBase::prepare(def, folder);
        ImagePool::OwnerGuard g(owner_id_);
        return prepare_fn_(inst_, def.c_str(), folder.c_str()) == 0;
    }

    // commit swaps staging → live. Gated (CallScope): a lone commit while the
    // pipeline runs is serialized vs process() for a non-reentrant plugin; under
    // a host commit_group the dispatch is already drained, so it's uncontended.
    // No export → no-op (a plugin with no double-slot already swapped in set_def).
    void commit() override {
        if (!commit_fn_ || !inst_) return;
        ImagePool::OwnerGuard g(owner_id_);
        CallScope cs(this);
        commit_fn_(inst_);
    }

    void* raw_instance() const { return inst_; }
    xi_plugin_process_fn process_fn() const { return process_fn_; }
    bool reentrant() const { return reentrant_; }
    // γ: true ⇒ caller may set xi_record.doc (borrowed yyjson doc) instead of
    // serializing to data/len. False ⇒ JSON path (foreign/older plugin).
    bool doc_input_ok() const { return doc_input_ok_; }

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
    xi_plugin_prepare_fn  prepare_fn_ = nullptr;   // ABI v7, optional
    xi_plugin_commit_fn   commit_fn_  = nullptr;   // ABI v7, optional
    bool                  doc_input_ok_ = false;
    ImagePoolOwnerId      owner_id_ = 0;
};

} // namespace xi
