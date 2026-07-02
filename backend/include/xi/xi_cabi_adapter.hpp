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
#include "xi_fault_policy.hpp"    // OnFault (per-instance post-fault policy, item 14)
#include "xi_image_pool.hpp"
#include "xi_instance.hpp"
#include "xi_record.hpp"          // γ: yyjson_layout_stamp() for the doc-pointer gate
#include "xi_record_schema.hpp"   // OQ-7: opt-in static Record field contract

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace xi {

// Plugin ABI compatibility check. Two gates, run at load (caller FreeLibrary +
// skip + record the warning on a false return):
//   1. ABI VERSION — reads xi_plugin_abi_version(); a plugin requesting a newer
//      ABI than the host provides is refused, as is one OLDER than
//      XI_ABI_MIN_COMPAT (built against a pre-layout-break xi_host_api — see the
//      macro in xi_abi.h). A pre-versioning plugin (no export) is treated as v1,
//      so it too falls below the floor and is refused.
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
    if (v < XI_ABI_MIN_COMPAT) {
        if (err_msg) *err_msg = "plugin '" + plugin_name + "' was built against ABI v"
                              + std::to_string(v) + ", older than the host's minimum v"
                              + std::to_string(XI_ABI_MIN_COMPAT) + " (a breaking "
                                "xi_host_api layout change since then would corrupt "
                                "memory) — rebuild it against the current ABI v"
                              + std::to_string(XI_ABI_VERSION);
        return false;
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

// One {iface, min} entry of a plugin.json `requires[]` / `optional[]` array —
// the LV2-style capability handshake (core_fix_plan.md §11 LV2 row / §12 Phase
// 3). `iface` is a capability id resolved through the host's get_interface door
// (e.g. "xi.imaging"); `min` is the lowest interface version the plugin accepts.
// A REQUIRED entry the host can't satisfy (no such id, or only an older version)
// refuses the load with a reason; an OPTIONAL entry never gates — the plugin
// null-checks it at runtime (mirrors LV2_Feature required vs optional).
struct IfaceReq {
    std::string iface;
    uint32_t    min = 1;
};

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
    // Ordered output sink (plugin.json `"sink": true` or `"role": "sink"`). A
    // script's xi::use(<this instance>).process(rec) is then NOT run inline during
    // inspect — the host STAGES it and flushes after the inspect inside the ordered-
    // emit gate, so the sink's side effect (comm → PLC, expose push, …) lands in
    // FRAME order even under parallel dispatch. Fire-and-forget: the process()
    // reply is dropped. See run_one_inspection / docs/reference/c-abi.md.
    bool        is_sink = false;
    std::string folder_path;   // absolute path to plugin folder
    std::string ui_path;       // absolute path to ui/ folder (if has_ui)
    HMODULE     handle = nullptr;
    // On-disk write-time of the DLL we loaded — the change-gate for
    // reload_changed: a rebuild only hot-swaps when a source is newer than this.
    uint64_t    loaded_dll_mtime = 0;

    // Optional. If `plugin.json` has a top-level `manifest` object, its
    // raw JSON text lands here. The backend doesn't validate or reshape
    // it — clients (AI agents, doc tools) parse the content themselves.
    // Convention (free-form): `params` / `inputs` / `outputs` / `exchange`
    // arrays describing what the plugin tunes, consumes, produces, and
    // accepts via exchange_instance. See docs/reference/c-abi.md.
    std::string manifest_json;

    // LV2-style capability handshake (core_fix_plan.md §11/§12 Phase 3). Parsed
    // from plugin.json's optional `"requires":[{"iface","min"}]` (gated at load)
    // and `"optional":[...]` (NOT gated — runtime null-check). Empty for the vast
    // majority of plugins, which depend only on the always-present legacy surface.
    std::vector<IfaceReq> required_ifaces;
    std::vector<IfaceReq> optional_ifaces;

    // Post-fault policy DEFAULT (plugin.json `"on_fault"`: "reuse" | "reinit" |
    // "refuse"). item 14 — what happens to an instance whose process() faults and
    // is caught. `reuse` (the default) keeps today's behavior. A per-instance
    // instance.json `"on_fault"` overrides this. See xi_fault_policy.hpp.
    OnFault default_on_fault = OnFault::Reuse;

    // New C ABI factory: void* (host_api, name)
    using CFactoryFn = void* (*)(const xi_host_api* host, const char* name);
    CFactoryFn c_factory = nullptr;
};

// Does the host publish capability `id` at some version >= `min` through its
// get_interface door? The door answers an EXACT (id, version); a published
// (id, vN) is frozen forever and additive, so a bounded upward probe from `min`
// honours the LV2 ">= min" semantics (and tolerates an interface that was born
// above the requested min). No host / no door → not published (refuse).
inline bool host_publishes_iface(const xi_host_api* host,
                                 const std::string& id, uint32_t min) {
    if (!host || !host->get_interface || id.empty()) return false;
    // 256-wide window: trivial at load time, bounded, and far beyond any
    // plausible live interface-version count.
    for (uint32_t v = min; v < min + 256u; ++v)
        if (host->get_interface(id.c_str(), v) != nullptr) return true;
    return false;
}

// LV2-style required/optional capability handshake, run at load right after the
// ABI gate (core_fix_plan.md §11 LV2 row / §12 Phase 3). Every REQUIRED
// {iface,min} must be published by the host's get_interface door at a version
// >= min; a missing or too-old required interface is a CLEAN REFUSE with a
// reason, surfaced through the same err_msg channel as plugin_abi_compatible (so
// the loader's existing refuse-with-reason path carries it). OPTIONAL interfaces
// are NEVER gated here — the plugin null-checks them at runtime via
// get_interface. Returns true (load) when every required capability is present.
inline bool plugin_caps_compatible(const PluginInfo& pi, const xi_host_api* host,
                                   const std::string& plugin_name,
                                   std::string* err_msg = nullptr) {
    for (const auto& r : pi.required_ifaces) {
        if (!host_publishes_iface(host, r.iface, r.min)) {
            if (err_msg) *err_msg =
                "plugin '" + plugin_name + "' requires capability '" + r.iface
                + "@" + std::to_string(r.min) + "' but the host does not publish it "
                  "(missing capability, or only an older version than requested) — "
                  "rebuild against a host that provides '" + r.iface + "@"
                + std::to_string(r.min) + "', or drop it from the plugin.json "
                  "\"requires\" list if the capability is in fact optional";
            return false;
        }
    }
    return true;
}

// ===========================================================================
// G3.2 — debug-build lifecycle×thread contract enforcement (core_fix_plan-2026-07
// §18 G3.2). The G3.1 contract (docs/guides/write-a-plugin.md § "Plugin lifecycle
// & threading contract") is machine-checked here, but ONLY in Debug: the whole
// apparatus lives behind `#ifndef NDEBUG` and compiles to nothing in Release.
//
// What is *actually* illegal (and not otherwise prevented) is narrow. The
// `CallScope` admission gate already SERIALIZES cross-thread config-vs-process on
// a non-reentrant instance — a `set_def` arriving on the control thread while a
// dispatch worker runs `process` simply BLOCKS; it is legal and safe (proven by
// tests/test_set_def_race.cpp). The one transition the gate cannot make safe is a
// SAME-THREAD re-entry into a non-reentrant instance's gated export — e.g. a
// plugin's `process()` body re-entering the host to call `set_def()` / `commit()`
// / `process()` on its OWN instance. `CallScope` (cap=1) would then wait on a slot
// this very thread holds → a silent DEADLOCK. This guard converts that hang into a
// loud, catchable contract violation. It is exactly the plan's two named cases:
//   • "set_def() during process() on a non-reentrant instance" — process→set_def
//   • "process() before commit() (out-of-order lifecycle)"     — process→commit /
//     process→process re-entry, an out-of-order nested lifecycle call.
// A `reentrant=true` instance lifts the gate and owns its locking, so re-entry is
// NOT flagged there.
#ifndef NDEBUG
// Swappable violation sink. Default aborts (like assert); tests install a recorder
// so an illegal transition can be verified without killing the process.
using LifecycleViolationFn = void (*)(const char* what, const char* detail);
inline LifecycleViolationFn& lifecycle_violation_handler() {
    static LifecycleViolationFn fn = nullptr;   // null → default abort
    return fn;
}
inline void raise_lifecycle_violation(const char* what, const char* detail) {
    if (LifecycleViolationFn h = lifecycle_violation_handler()) { h(what, detail); return; }
    std::fprintf(stderr,
        "[xinsp2] FATAL: plugin lifecycle contract violation: %s (%s)\n"
        "  a non-reentrant instance was re-entered on the same thread; see "
        "docs/guides/write-a-plugin.md \"Plugin lifecycle & threading contract\"\n",
        what, detail);
    std::abort();
}
#endif // NDEBUG

// Adapter: wraps a C ABI plugin instance as an InstanceBase
class CAbiInstanceAdapter : public InstanceBase {
public:
    CAbiInstanceAdapter(std::string name, std::string plugin_name,
                        HMODULE dll, void* inst, bool reentrant = false,
                        int max_concurrency = 0, bool is_sink = false)
        : name_(std::move(name)), plugin_name_(std::move(plugin_name)),
          dll_(dll), inst_(inst), reentrant_(reentrant), is_sink_(is_sink),
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
        // OQ-7 (optional): capture the plugin's declared Record field contract at
        // LOAD time, once, so a composer can validate the wired pipeline up front
        // (see xi_record_schema.hpp). Absent export → schema stays undeclared and
        // the plugin keeps its current schemaless behaviour.
        if (auto schema_fn = reinterpret_cast<xi_plugin_record_schema_fn>(
                GetProcAddress(dll_, "xi_plugin_record_schema"))) {
            std::vector<char> buf(4096);
            int n = schema_fn(buf.data(), (int)buf.size());
            // ABI: callee returns -(exact content size). Alloc n+1: content + room for the trailing NUL a get_def-style export may write.
            if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1); n = schema_fn(buf.data(), (int)buf.size()); }
            if (n > 0) record_schema_ = parse_record_schema_json(buf.data(), (size_t)n);
        }
        // γ: may we hand this plugin a borrowed yyjson_mut_doc* (in-process zero-
        // serialize input)? Only if it was built against our yyjson layout.
        if (auto abi_fn = reinterpret_cast<uint32_t(*)()>(GetProcAddress(dll_, "xi_yyjson_abi")))
            doc_input_ok_ = (abi_fn() == xi::yyjson_layout_stamp());
    }

    ~CAbiInstanceAdapter() override {
        if (destroy_fn_ && inst_) destroy_fn_(inst_);
        // Sweep any image handles the plugin allocated and forgot to
        // release. Without this, plugin crashes / careless authors leak
        // ImagePool entries forever. GUARD g_image_pool_alive: if this adapter is
        // destroyed during STATIC destruction (a never-closed project reaching
        // ~PluginManager) the ImagePool Meyers singleton may already be gone —
        // ImagePool::instance() would return (and re-flag alive on) a destroyed
        // object and release_all_for would iterate freed slots. Skip the sweep then;
        // the process is exiting and the OS reclaims the pool memory anyway.
        if (!g_image_pool_alive.load(std::memory_order_acquire)) return;
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
#ifndef NDEBUG
        LcGate lc(this, "get_def"); if (!lc.ok()) return "{}";
#endif
        ImagePool::OwnerGuard g(owner_id_);
        CallScope cs(this);
        std::vector<char> buf(4096);
        int n = get_def_fn_(inst_, buf.data(), (int)buf.size());
        // ABI: callee returns -(exact content size). Alloc n+1: content + the trailing NUL the export writes.
        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1); n = get_def_fn_(inst_, buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    bool set_def(const std::string& j) override {
        if (!set_def_fn_ || !inst_) return false;
#ifndef NDEBUG
        LcGate lc(this, "set_def"); if (!lc.ok()) return false;
#endif
        ImagePool::OwnerGuard g(owner_id_);
        CallScope cs(this);
        bool ok = set_def_fn_(inst_, j.c_str()) == 0;
        // item 14: remember the last accepted config so an on_fault=reinit rebuild
        // can restore it onto a freshly-created instance (dropping the in-flight
        // state a fault may have corrupted). Cached under the same gate as the call.
        if (ok) committed_def_ = j;
        return ok;
    }

    std::string exchange(const std::string& cmd_json) override {
        if (!exchange_fn_ || !inst_) return "{}";
#ifndef NDEBUG
        LcGate lc(this, "exchange"); if (!lc.ok()) return "{}";
#endif
        ImagePool::OwnerGuard g(owner_id_);
        CallScope cs(this);
        std::vector<char> buf(64 * 1024);
        int n = exchange_fn_(inst_, cmd_json.c_str(), buf.data(), (int)buf.size());
        // ABI: callee returns -(exact content size). Alloc n+1: content + the trailing NUL the export writes.
        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1); n = exchange_fn_(inst_, cmd_json.c_str(), buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    // Run the plugin's process() entry point. Wraps the OwnerGuard (image-leak
    // tagging) and, for a non-reentrant plugin, the per-instance lock so a
    // parallel dispatch pool can't re-enter the same instance's state
    // concurrently. Returns output->image_count, or -1 if no process fn.
    // The caller owns the SEH try/catch boundary (use_process_cb).
    int process(const xi_record* in, xi_record_out* out) {
        if (!process_fn_ || !inst_) return -1;
#ifndef NDEBUG
        LcGate lc(this, "process"); if (!lc.ok()) return -1;
#endif
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
        bool ok = prepare_fn_(inst_, def.c_str(), folder.c_str()) == 0;
        // item 14: the staged config is what commit() will make live — cache it
        // so a later on_fault=reinit rebuild restores this config (see set_def).
        if (ok) committed_def_ = def;
        return ok;
    }

    // commit swaps staging → live. Gated (CallScope): a lone commit while the
    // pipeline runs is serialized vs process() for a non-reentrant plugin; under
    // a host commit_group the dispatch is already drained, so it's uncontended.
    // No export → no-op (a plugin with no double-slot already swapped in set_def).
    void commit() override {
        if (!commit_fn_ || !inst_) return;
#ifndef NDEBUG
        LcGate lc(this, "commit"); if (!lc.ok()) return;
#endif
        ImagePool::OwnerGuard g(owner_id_);
        CallScope cs(this);
        commit_fn_(inst_);
    }

    // OQ-7: the plugin's declared cross-plugin Record field contract, captured at
    // load. .declared == false when the plugin exported no xi_plugin_record_schema
    // (the opt-in default). Feed a set of these (in pipeline order) to
    // xi::validate_record_pipeline for wire-time contract checking.
    const RecordSchema& record_schema() const { return record_schema_; }

    void* raw_instance() const { return inst_; }
    xi_plugin_process_fn process_fn() const { return process_fn_; }
    bool reentrant() const { return reentrant_; }
    bool is_sink()   const { return is_sink_; }   // ordered output sink (see PluginInfo::is_sink)
    // γ: true ⇒ caller may set xi_record.doc (borrowed yyjson doc) instead of
    // serializing to data/len. False ⇒ JSON path (foreign/older plugin).
    bool doc_input_ok() const { return doc_input_ok_; }

    // ---- item 14: post-fault policy + quarantine surface --------------------
    // The mechanical primitives the service-layer fault boundary (use_process_
    // inline_) drives; the health-overlay + escalation POLICY lives there, this
    // adapter just carries the per-instance state and provides the safe in-place
    // rebuild. Kept here because the per-instance CallScope gate — the natural
    // serialization point — already lives on the adapter.
    OnFault on_fault() const { return on_fault_; }
    void set_on_fault(OnFault p) { on_fault_ = p; }

    // The refuse fail-fast gate: a single relaxed atomic load, cheap enough to
    // sit on the (non-fault) hot path. True ⇒ the instance is quarantined and
    // process()/exchange() must fail fast without entering plugin code.
    bool quarantined() const { return quarantined_.load(std::memory_order_acquire); }
    void set_quarantined(bool q) { quarantined_.store(q, std::memory_order_release); }

    // on_fault=reinit request bit: set by the fault boundary on a caught fault,
    // consumed just before the next process() so the rebuild happens off no-frame.
    bool reinit_pending() const { return reinit_pending_.load(std::memory_order_acquire); }
    void request_reinit() { reinit_pending_.store(true, std::memory_order_release); }

    // Arm the in-place rebuild with the DLL factory + host so reinit() can
    // reconstruct this instance's plugin object. Called by the PM at each
    // (re)construction site (it owns the factory pointer). If never armed, an
    // on_fault=reinit degrades to reuse (documented) — a safe fallback.
    void arm_reinit(PluginInfo::CFactoryFn factory, const xi_host_api* host) {
        reinit_factory_ = factory; reinit_host_ = host;
    }

    // Consecutive-rebuild-failure accounting (escalation to refuse after
    // kReinitEscalateAfter). Touched only on the rare fault/reinit path, but from a
    // dispatch worker (bump) and the control thread (reset via re-enable), so atomic.
    int  note_reinit_fail() { return reinit_fails_.fetch_add(1, std::memory_order_relaxed) + 1; }
    void reset_reinit_fails() { reinit_fails_.store(0, std::memory_order_relaxed); }

    // Rebuild this instance from its last committed config, DROPPING the in-flight
    // persistent state a caught fault may have corrupted. Reuses the same
    // create → set_def steps as the PM reload path (make_adapter_guarded_), but in
    // place on THIS adapter so the shared_ptr other workers hold stays valid, and
    // SERIALIZED by CallScope so no process()/exchange() runs concurrently on this
    // instance. Returns true on a clean rebuild; false leaves the OLD instance
    // live (corrupt but runnable), mirroring recompile's restore-against-old.
    // Clears the reinit-pending bit regardless.
    bool reinit() {
        reinit_pending_.store(false, std::memory_order_release);
        if (!reinit_factory_ || !reinit_host_) return false;   // not armed → reuse
        CallScope cs(this);                 // serialize vs process/exchange/set_def
        void* fresh = nullptr;
        {
            ImagePool::OwnerGuard og(owner_id_);   // tag the ctor's images to us
            try { fresh = reinit_factory_(reinit_host_, name_.c_str()); }
            catch (...) { fresh = nullptr; }       // SEH-translated ctor fault, or throw
        }
        if (!fresh) return false;                  // keep the old instance live
        void* old = inst_;
        inst_ = fresh;                             // swap BEFORE destroying old
        if (destroy_fn_ && old) { try { destroy_fn_(old); } catch (...) {} }
        // Restore the last committed config onto the fresh instance. (We do NOT
        // sweep the old instance's leaked pool images here — both share owner_id_,
        // so a sweep would also free the fresh ctor's images; the rare residual is
        // reclaimed when the adapter is finally destroyed.)
        if (set_def_fn_ && !committed_def_.empty()) {
            ImagePool::OwnerGuard og(owner_id_);
            try { set_def_fn_(inst_, committed_def_.c_str()); } catch (...) {}
        }
        return true;
    }

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

#ifndef NDEBUG
    // G3.2 debug lifecycle guard. Per-thread stack of the instances this thread is
    // currently inside a GATED export on. A gated entry that finds THIS instance
    // already on THIS thread's stack (and the instance is non-reentrant) is the
    // deadlock-inducing same-thread re-entry the contract forbids. thread_local ⇒
    // no locks, and cross-thread concurrency (legal, gate-serialized) never
    // appears on another thread's stack, so it is correctly NOT flagged.
    static std::vector<const void*>& lc_tls_stack_() {
        thread_local std::vector<const void*> s;
        return s;
    }
    // Returns true if the gated entry is legal (caller proceeds into CallScope and
    // must pair it with lc_leave_()). Returns false after raising a violation —
    // the caller must BAIL immediately, never entering CallScope (which would
    // deadlock on the slot this thread already holds).
    bool lc_enter_(const char* who) const {
        if (!reentrant_) {
            for (const void* p : lc_tls_stack_())
                if (p == this) { raise_lifecycle_violation("non-reentrant re-entry", who);
                                 return false; }
        }
        lc_tls_stack_().push_back(this);
        return true;
    }
    void lc_leave_() const {
        auto& s = lc_tls_stack_();
        for (auto it = s.rbegin(); it != s.rend(); ++it)
            if (*it == this) { s.erase(std::next(it).base()); return; }
    }
    // RAII: on legal entry pushes/pops the thread's gated stack; `ok()` is false
    // when the entry was an illegal re-entry (violation already raised).
    struct LcGate {
        const CAbiInstanceAdapter* a_; bool ok_;
        LcGate(const CAbiInstanceAdapter* a, const char* who) : a_(a), ok_(a->lc_enter_(who)) {}
        ~LcGate() { if (ok_) a_->lc_leave_(); }
        bool ok() const { return ok_; }
        LcGate(const LcGate&) = delete; LcGate& operator=(const LcGate&) = delete;
    };
#endif // NDEBUG

    mutable std::mutex              cc_mu_;
    mutable std::condition_variable cc_cv_;
    mutable int                     cur_calls_ = 0;
    bool reentrant_ = false;
    bool is_sink_   = false;
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
    RecordSchema          record_schema_;          // OQ-7, optional (declared==false ⇒ none)
    bool                  doc_input_ok_ = false;
    ImagePoolOwnerId      owner_id_ = 0;

    // ---- item 14: post-fault policy state -----------------------------------
    OnFault                on_fault_ = OnFault::Reuse;   // effective policy (from PM)
    std::atomic<bool>      quarantined_{false};          // refuse gate (hot path)
    std::atomic<bool>      reinit_pending_{false};       // deferred-rebuild request
    std::atomic<int>       reinit_fails_{0};             // consecutive rebuild failures
    PluginInfo::CFactoryFn reinit_factory_ = nullptr;    // armed by the PM
    const xi_host_api*     reinit_host_ = nullptr;       // armed by the PM
    std::string            committed_def_;               // last accepted config (for reinit)
};

} // namespace xi
