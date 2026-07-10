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

#include "xi_dynlib.hpp"          // HMODULE/LoadLibrary/GetProcAddress/FreeLibrary (win32 or dlopen shim)

#include "xi_abi.h"
#include "xi_cap_guard.hpp"       // capability plane: data-plane mark (doc 14 pilot)
#include "xi_fault_policy.hpp"    // OnFault (per-instance post-fault policy, item 14)
#include "xi_image_pool.hpp"
#include "xi_instance.hpp"

#include <atomic>
#include <condition_variable>
#include <shared_mutex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace xi {

// Plugin ABI compatibility check — the ABI VERSION gate, run at load (caller
// FreeLibrary + skip + record the warning on a false return). Reads
// xi_plugin_abi_version(): a plugin requesting a newer ABI than the host
// provides is refused, as is one OLDER than XI_ABI_MIN_COMPAT (built against a
// pre-layout-break xi_host_api — see the macro in xi_abi.h). A pre-versioning
// plugin (no export) is treated as v1, so it too falls below the floor and is
// refused.
//
// [ABI v12 — THE CUT] The former γ-4 yyjson-LAYOUT gate is GONE: a v12 plugin
// has no yyjson doc path (the Record doc-pointer dispatch was deleted; the data
// plane is the xi.pack@1 door), so there is nothing to gate on. The
// `json_fallback_opt_in` parameter is retained for ABI/call-site stability but
// no longer consulted. A plugin loads iff its ABI version passes min-compat and
// it publishes what it needs (the required-capability handshake below).
inline bool plugin_abi_compatible(HMODULE dll, const std::string& plugin_name,
                                  bool json_fallback_opt_in,
                                  std::string* err_msg = nullptr) {
    (void)json_fallback_opt_in;   // THE CUT: no yyjson-layout gate remains
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
    // See docs/guides/write-a-script.md (parallelism) + docs/guides/write-a-plugin.md.
    bool        reentrant = false;
    // [ABI v12 — THE CUT] Vestigial. Parsed from plugin.json `"json_fallback"`
    // for manifest back-compat, but NO LONGER CONSULTED: the yyjson-layout load
    // gate it opted out of was deleted with the Record doc path (a v12 plugin's
    // data plane is the xi.pack@1 door — no yyjson doc crosses the ABI). Kept as
    // a field so an older manifest carrying the key still parses cleanly.
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
    // Informational lib-plugin marker (plugin.json `"lib": true`, doc 14): a
    // capability provider with NO data plane — never wired as a pipeline input/
    // output. Parsed for documentation + as a sanity signal; the autoload gate
    // below is the operative flag.
    bool        is_lib = false;
    // V3 machine-level autoload (plugin.json `"autoload": true`, doc 14/19 V3).
    // A `lib` capability provider marked autoload is instantiated ONCE at service
    // boot under a stable MACHINE owner (PluginManager::machine_instances_), so
    // its capabilities are registered WITHOUT any project declaring a per-instance
    // (E1 second cause, doc 06 §6). Machine-scoped: it persists across project
    // open/close. A project instance of the same plugin still takes precedence —
    // it evicts the machine provider before its own factory runs (no double-
    // register), and the machine provider is reinstated when the project provider
    // goes away. Only meaningful together with `lib` (a data-plane plugin has no
    // capabilities to autoload).
    bool        autoload = false;
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
        // ABI v7 (optional — present only if the plugin opted in with
        // XI_PLUGIN_STAGED). Their presence IS the opt-in signal: a plugin that
        // exports prepare promises it touches only its staging slot, so we may
        // call it ungated (concurrent with process). Absent → gated set_def / no-op.
        prepare_fn_  = reinterpret_cast<xi_plugin_prepare_fn>(GetProcAddress(dll_, "xi_plugin_prepare"));
        commit_fn_   = reinterpret_cast<xi_plugin_commit_fn>(GetProcAddress(dll_, "xi_plugin_commit"));
        // prepare/commit are a CONTRACT PAIR — XI_PLUGIN_STAGED exports both, and the
        // staged path is only coherent when both are present: prepare() stages into the
        // background slot and commit() swaps it live. A plugin that exports exactly ONE
        // tears its config either way: commit-only → prepare falls to the gated
        // InstanceBase::prepare (immediate-live), then commit() swaps a never-prepared
        // staging slot over live (torn/reverted config, yet commit_group still ok:true);
        // prepare-only → commit() no-ops, so staged config silently never goes live.
        // Fail loud AND make it safe: null BOTH so the instance degrades to the coherent
        // gated InstanceBase::prepare(=set_def)/commit(=no-op) path — an immediate-live
        // swap with no torn double-slot — instead of the half-wired staged path.
        if ((prepare_fn_ != nullptr) != (commit_fn_ != nullptr)) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true)) {
                std::fprintf(stderr,
                    "[xinsp2] plugin '%s' exports only %s of the xi_plugin_prepare/"
                    "xi_plugin_commit pair — CONTRACT VIOLATION; disabling the staged "
                    "config path for it (falling back to the gated set_def path). "
                    "Export BOTH (XI_PLUGIN_STAGED) or NEITHER.\n",
                    plugin_name_.c_str(),
                    prepare_fn_ ? "xi_plugin_prepare" : "xi_plugin_commit");
            }
            prepare_fn_ = nullptr;
            commit_fn_  = nullptr;
        }
        // polaris2 wave-2 (synthesis §3 pure-door dry run): the plugin's OWN
        // capability door — the symmetric mirror of host->get_interface, resolved
        // via GetProcAddress like prepare/commit (ABI-additive). Probe it once for
        // the pack process capability. A plugin that answers xi.pack@1 speaks
        // pack-in/pack-out; NULL = it publishes no data-plane door.
        plugin_get_iface_fn_ = reinterpret_cast<xi_plugin_get_interface_fn>(
            GetProcAddress(dll_, "xi_plugin_get_interface"));
        if (plugin_get_iface_fn_)
            frame_proc_ = static_cast<const xi_pack_proc_v1*>(
                plugin_get_iface_fn_("xi.pack", 1));
    }

    ~CAbiInstanceAdapter() override {
        if (destroy_fn_ && inst_) {
            // OwnerGuard so the plugin dtor's own releases (image handles AND
            // pack refs — cache-style retaining plugins release their ring
            // here) are attributed to this instance, exactly like every other
            // entry point. The sweeps below then reclaim only what the plugin
            // genuinely forgot.
            ImagePool::OwnerGuard g(owner_id_);
            destroy_fn_(inst_);
        }
        // Capability-plane sweep (doc 14 pilot; slot bridge — null until
        // install_cap_plane): drop any capability registrations this instance
        // still provides, so a lib plugin that forgot to unregister on destroy
        // can never leave a dangling handler/self in the registry. Runs AFTER
        // destroy_fn_ (a well-behaved plugin's own unregister has already
        // emptied its bucket). Safe during static destruction: the cap
        // registry singleton is intentionally leaked (xi_cap_abi.hpp).
        int cswept = ImagePool::sweep_caps_for(owner_id_);
        if (cswept > 0) {
            std::fprintf(stderr,
                "[xinsp2] '%s' destroyed; swept %d leaked capability registration(s)\n",
                name_.c_str(), cswept);
        }
        // Sweep any image handles the plugin allocated and forgot to
        // release. Without this, plugin crashes / careless authors leak
        // ImagePool entries forever. Safe even when this adapter is destroyed
        // during STATIC destruction (a never-closed project reaching
        // ~PluginManager): the ImagePool singleton is intentionally leaked, so
        // it is always alive here. Note this means the pack sweep below also
        // ALWAYS runs — a caller-owned pack a plugin produced is protected by
        // the ownership handoff (run_pack_door / the cap funnel retag its
        // output ref untagged), so the sweep reclaims only genuine leaks.
        int swept = ImagePool::instance().release_all_for(owner_id_);
        if (swept > 0) {
            std::fprintf(stderr,
                "[xinsp2] '%s' destroyed; swept %d leaked image handle(s)\n",
                name_.c_str(), swept);
        }
        // Pack-plane analogue (via the slot bridge — null until the pack ABI is
        // installed): reclaim sealed-pack refs this instance retained and forgot
        // to release, so a careless pack-retaining plugin can't silently leak
        // packs in the registry until static teardown.
        int fswept = ImagePool::sweep_packs_for(owner_id_);
        if (fswept > 0) {
            std::fprintf(stderr,
                "[xinsp2] '%s' destroyed; swept %d leaked pack ref(s)\n",
                name_.c_str(), fswept);
        }
    }

    ImagePoolOwnerId owner_id() const { return owner_id_; }
    // Replace the auto-allocated id with one the caller pre-allocated
    // (used by open_project / create_instance to tag handles the
    // plugin's ctor allocates BEFORE the adapter exists). The
    // sweep-on-destroy still sees the right bucket either way.
    void adopt_owner_id(ImagePoolOwnerId id) { owner_id_ = id; }

    const std::string& name() const override { return name_; }
    const std::string& plugin_name() const override { return plugin_name_; }

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
        // B6 (burr audit): per-thread scratch instead of a fresh 64 KiB vector
        // per call — steady-state exchange() calls stop allocating (grow-only,
        // never shrunk). RE-ENTRANCY VERDICT: safe. exchange() is entered only
        // host-side (WS cmd_exchange_instance_, the script's use_exchange_cb,
        // project save/load) — xi_host_api publishes NO door through which a
        // plugin's exchange handler could re-enter another instance's
        // exchange() on the same thread (the cap funnel invokes provider
        // handlers directly, never via the adapter), so the scratch cannot be
        // clobbered under our own stack frame. If such a door is ever added,
        // this needs a TLS depth counter falling back to a local vector.
        thread_local std::vector<char> buf;
        if (buf.size() < 64 * 1024) buf.resize(64 * 1024);
        int n = exchange_fn_(inst_, cmd_json.c_str(), buf.data(), (int)buf.size());
        // ABI: callee returns -(exact content size). Alloc n+1: content + the trailing NUL the export writes.
        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1); n = exchange_fn_(inst_, cmd_json.c_str(), buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    // polaris2 wave-2: does this plugin publish the xi.pack@1 pack-in/pack-out
    // door (resolved once at construction via xi_plugin_get_interface)?
    bool has_pack_door() const { return frame_proc_ && frame_proc_->process; }

    // Drive the plugin's pack door under the SAME per-instance discipline as
    // process() (OwnerGuard image-leak tagging + CallScope serialization for a
    // non-reentrant instance). `in` is a sealed host pack handle (borrowed); the
    // return is a NEW sealed pack handle the caller owns (host pack registry).
    // XI_PACK_NULL when the plugin has no door. Caller owns the SEH boundary,
    // exactly as with process().
    xi_pack_handle run_pack_door(xi_pack_handle in) {
        if (!has_pack_door() || !inst_) return XI_PACK_NULL;
#ifndef NDEBUG
        LcGate lc(this, "process"); if (!lc.ok()) return XI_PACK_NULL;
#endif
        ImagePool::OwnerGuard og(owner_id_);
        CallScope cs(this);
        DataPlaneMark dp;   // capability plane: no registration from inside a door
        xi_pack_handle out = frame_proc_->process(inst_, in);
        // Ownership handoff — the same fix as the capability funnel
        // (xi_pack_abi.hpp): the door sealed its output under
        // OwnerGuard(owner_id_), stamping THIS producing instance as creator,
        // but the seal ref is handed to the CALLER, who owns it. Left tagged,
        // this instance's teardown sweep (dtor → sweep_packs_for) would reclaim
        // the handed-off ref and free the caller's live pack. Clear the creator
        // tag while the OwnerGuard/CallScope pin is still held — rc unchanged
        // (PackRegistry::untag). Every door caller (use_pack_process_cb, the
        // runner, tests) routes through here, so this is the one central spot.
        if (out != XI_PACK_NULL)
            ImagePool::untag_pack_ref(out, owner_id_);
        return out;
    }

    // Frame-perfect config swap (ABI v7). prepare loads the new config's heavy
    // assets into the plugin's background staging slot; it runs UNGATED — NO
    // CallScope — so it proceeds concurrent with process() and never stalls the
    // pipeline. That is sound ONLY because a plugin that exports xi_plugin_prepare
    // (via XI_PLUGIN_STAGED) contracts to touch staging ONLY. A plugin without
    // the export falls back to the base prepare ≡ set_def, which IS gated (safe).
    //
    // HAZARD (doc 25 RT3-B1): this ungated entry reads inst_ with NO cap_gate_, so it
    // would race reinit()'s destroy_fn_(old) → UAF. That combo (staged-prepare +
    // on_fault=reinit) is DEFUSED at load — set_on_fault downgrades reinit→reuse for a
    // prepare-exporting (or reentrant) instance — so reinit() never runs here. If that
    // guard is ever lifted, prepare() MUST take shared_lock(cap_gate_) + re-read inst_.
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

    void* raw_instance() const { return inst_; }
    bool reentrant() const { return reentrant_; }
    bool is_sink()   const { return is_sink_; }   // ordered output sink (see PluginInfo::is_sink)

    // ---- item 14: post-fault policy + quarantine surface --------------------
    // The mechanical primitives the service-layer fault boundary (use_process_
    // inline_) drives; the health-overlay + escalation POLICY lives there, this
    // adapter just carries the per-instance state and provides the safe in-place
    // rebuild. Kept here because the per-instance CallScope gate — the natural
    // serialization point — already lives on the adapter.
    OnFault on_fault() const { return on_fault_; }
    // RT3-B1/B2 (doc 25) COMBO GUARD: on_fault=reinit destroys the old inst_ under
    // the EXCLUSIVE cap_gate_, but that gate is taken ONLY by the capability funnel.
    // Two data-plane readers it never covers turn reinit into a use-after-free during
    // fault recovery: (1) the UNGATED prepare() staging entry (a plugin exporting
    // xi_plugin_prepare), and (2) any entry on a REENTRANT instance (CallScope is a
    // no-op when effective_cap_()==0). These are dangerous config COMBINATIONS that
    // nothing in-tree uses. Rather than make the race safe (a shared_lock in prepare()
    // vs reinit()'s exclusive gate — real work + deadlock-review risk), DEFUSE the
    // combo at load: refuse reinit for such an instance, fall back to the already-
    // documented safe policy (reuse), and warn loudly. If the combo is ever a real
    // need, fix the race and lift this guard — never silently run the UAF path.
    void set_on_fault(OnFault p) {
        if (p == OnFault::Reinit && (reentrant_ || prepare_fn_)) {
            std::fprintf(stderr,
                "[xinsp2] WARN instance '%s' (plugin '%s'): on_fault=reinit is UNSAFE with %s "
                "— reinit's destroy races the %s (doc 25 RT3-B1/B2, a use-after-free). "
                "Downgrading on_fault to 'reuse'.\n",
                name_.c_str(), plugin_name_.c_str(),
                reentrant_ ? "a reentrant plugin" : "staged-prepare (xi_plugin_prepare)",
                reentrant_ ? "reentrant data plane" : "the ungated prepare() path");
            p = OnFault::Reuse;
        }
        on_fault_ = p;
    }

    // The refuse fail-fast gate: a single relaxed atomic load, cheap enough to
    // sit on the (non-fault) hot path. True ⇒ the instance is quarantined and
    // process()/exchange() must fail fast without entering plugin code.
    bool quarantined() const { return quarantined_.load(std::memory_order_acquire); }
    void set_quarantined(bool q) { quarantined_.store(q, std::memory_order_release); }

    // on_fault=reinit request bit: set by the fault boundary on a caught fault,
    // consumed just before the next process() so the rebuild happens off no-frame.
    bool reinit_pending() const { return reinit_pending_.load(std::memory_order_acquire); }
    void request_reinit() { reinit_pending_.store(true, std::memory_order_release); }

    // H5: atomically CONSUME the pending-rebuild request — a single test-and-clear.
    // When several callers fault concurrently on the SAME instance they all observe
    // reinit_pending()==true; routing the rebuild through this exchange makes exactly
    // ONE win (gets true) and perform the rebuild + escalation accounting, while the
    // losers get false and skip. That closes two windows the bare check-then-act had:
    // the DOUBLE rebuild (a 2nd reinit() destroying the 1st's fresh inst_ and building
    // a 3rd) and the escalation-ORDERING race (a failing rebuild's note_reinit_fail()
    // crossing the quarantine threshold before a succeeding rebuild's
    // reset_reinit_fails(), quarantining a now-healthy instance). reinit() still clears
    // the bit at its own entry (a direct reinit() consumes its request); after this
    // exchange that store is the idempotent no-op.
    bool consume_reinit_pending() { return reinit_pending_.exchange(false, std::memory_order_acq_rel); }

    // Arm the in-place rebuild with the DLL factory + host so reinit() can
    // reconstruct this instance's plugin object. Called by the PM at each
    // (re)construction site (it owns the factory pointer). If never armed, an
    // on_fault=reinit degrades to reuse (documented) — a safe fallback.
    void arm_reinit(PluginInfo::CFactoryFn factory, const xi_host_api* host) {
        reinit_factory_ = factory; reinit_host_ = host;
    }

    // A1/A2 (redteam doc 21) — the reinit gate. The capability funnel runs the
    // provider handler under NO CallScope (providers contract to be thread-safe),
    // so reinit()'s destroy_fn_(old) could free inst_ under a concurrent cap
    // handler — the shared_ptr pin protects the ADAPTER, not inst_. A cap call
    // takes the SHARED side of this gate around its handler run (and re-resolves
    // the live handler/self under it); reinit takes the EXCLUSIVE side around the
    // destroy of the old inst_, so the destroy waits out every in-flight handler
    // and no handler ever runs against a freed inst_. Reader-writer, not a plain
    // mutex, so concurrent HEAVY cap calls (doc 14 sizing) still run in parallel;
    // the exclusive lock is taken only on the rare fault/reinit path.
    std::shared_mutex& cap_reinit_gate() const { return cap_gate_; }

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
        // Capability plane (doc 14 pilot): the registry's handler/self pointers
        // belong to the CORRUPTED instance this rebuild replaces — sweep them
        // BEFORE the factory runs so nothing dangles, and so the fresh ctor's
        // re-registrations (same owner id) land in a clean bucket. If the
        // factory fails below, the old instance stays live but UNREGISTERED —
        // honest: a faulted lib whose rebuild failed stops serving (and the
        // escalation path quarantines it after kReinitEscalateAfter failures).
        ImagePool::sweep_caps_for(owner_id_);
        void* fresh = nullptr;
        {
            ImagePool::OwnerGuard og(owner_id_);   // tag the ctor's images to us
            try { fresh = reinit_factory_(reinit_host_, name_.c_str()); }
            catch (...) { fresh = nullptr; }       // SEH-translated ctor fault, or throw
        }
        if (!fresh) return false;                  // keep the old instance live
        void* old = inst_;
        inst_ = fresh;                             // swap BEFORE destroying old
        // A1/A2 (redteam doc 21): the cap funnel runs the provider handler with
        // no CallScope, so a concurrent cap handler may still be executing against
        // `old`. Take the EXCLUSIVE side of the reinit gate before destroying it —
        // this blocks until every in-flight cap handler (each holding the SHARED
        // side and re-resolving the live handler under it) has drained, so the
        // destroy can never free inst_ out from under a running handler.
        // SCOPE (doc 25 RT3-B1/B2): this exclusive gate covers ONLY the capability
        // funnel's SHARED-side readers. It does NOT cover the ungated prepare() entry
        // or a reentrant instance's data plane (CallScope is a no-op there) — those
        // would still race this destroy. They are DEFUSED at load (set_on_fault
        // downgrades reinit→reuse for a prepare-exporting/reentrant instance), so
        // reinit() is unreachable for them; do not rely on this gate to cover them.
        {
            std::unique_lock<std::shared_mutex> gate(cap_gate_);
            if (destroy_fn_ && old) { try { destroy_fn_(old); } catch (...) {} }
        }
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
    xi_plugin_prepare_fn  prepare_fn_ = nullptr;   // ABI v7, optional
    xi_plugin_commit_fn   commit_fn_  = nullptr;   // ABI v7, optional
    xi_plugin_get_interface_fn plugin_get_iface_fn_ = nullptr;  // wave-2, optional
    const xi_pack_proc_v1*    frame_proc_          = nullptr;  // xi.pack@1 door (null = absent)
    ImagePoolOwnerId      owner_id_ = 0;

    // ---- item 14: post-fault policy state -----------------------------------
    OnFault                on_fault_ = OnFault::Reuse;   // effective policy (from PM)
    std::atomic<bool>      quarantined_{false};          // refuse gate (hot path)
    std::atomic<bool>      reinit_pending_{false};       // deferred-rebuild request
    std::atomic<int>       reinit_fails_{0};             // consecutive rebuild failures
    PluginInfo::CFactoryFn reinit_factory_ = nullptr;    // armed by the PM
    const xi_host_api*     reinit_host_ = nullptr;       // armed by the PM
    std::string            committed_def_;               // last accepted config (for reinit)
    // A1/A2 reinit gate: cap calls take it SHARED, reinit's destroy takes it
    // EXCLUSIVE (see cap_reinit_gate()).
    mutable std::shared_mutex cap_gate_;
};

} // namespace xi
