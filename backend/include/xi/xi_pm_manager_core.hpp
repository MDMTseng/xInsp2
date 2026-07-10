#pragma once
//
// xi_pm_manager_core.hpp — the PluginManager class DECLARATION.
//
// This header carries the class shell: member function DECLARATIONS, the nested
// types (PendingInstance / RecompileResult / PluginRebuildReport / ExportResult /
// RenameResult), the data members, and the trivial inline accessors. The bodies
// of the substantial methods are defined out-of-class (as `inline
// PluginManager::method(...)`) in the concern headers:
//
//   xi_pm_discovery.hpp  — scan / certify / register / resolve externals
//   xi_pm_load.hpp       — DLL load / compile / recompile / rebuild / export
//   xi_pm_project.hpp    — create/close/open project + working-copy transactions
//   xi_pm_instances.hpp  — instance CRUD, lifecycle state, JSON persistence
//
// The umbrella <xi/xi_plugin_manager.hpp> includes this core + all four concern
// headers in dependency order, so every existing consumer sees the SAME symbols.
//
// This is a mechanical, behaviour-preserving split of the former 2787-line
// xi_plugin_manager.hpp: code was only MOVED between headers (method bodies out
// of the class body) — no logic, signature, ABI, or wire change.
//

#include "xi_dynlib.hpp"       // HMODULE/LoadLibrary/GetProcAddress/FreeLibrary (win32 or dlopen shim)

#include "xi_abi.h"
#include "xi_atomic_io.hpp"
#include "xi_clock.hpp"        // wall_ms() — portable timestamp for .corrupt-<ts> quarantine names
#include "xi_cabi_adapter.hpp" // plugin_abi_compatible / PluginInfo / CAbiInstanceAdapter
#include "xi_certify.hpp"     // Part III G1: scan/certification isolation (child-process certify + verdict cache)
#include "xi_image_pool.hpp"
#include "xi_pack_abi.hpp"   // polaris2 wave-2: install_pack_abi (xi.pack@1 door + dispatch releaser)
#include "xi_cap_abi.hpp"    // capability plane pilot (doc 14): install_cap_plane (xi.cap@1 + xi.cap.provider@1)
#include "xi_instance.hpp"
#include "xi_config_validate.hpp" // validate_config_against_manifest (opt-in diagnostic, extracted leaf)
#include "xi_pm_json.hpp"      // pm_json_escape (extracted leaf)
#include "xi_quiesce_token.hpp" // QuiesceToken — proof-of-quiesce capability for destructive methods
#include <cctype>
#include <cassert>            // door_matches_fields freeze-guard (default_host_api)
#include "xi_pm_parse.hpp"     // parse_manifest / extract_string / detail_find_key
#include "xi_plugin_export.hpp" // export_project_plugin_impl (deploy packaging, extracted leaf)
#include "xi_cmake_build.hpp"  // xi::cmake_build:: host-side cmake invocation (extracted leaf)
#include "xi_working_copy.hpp" // xi::wc:: transactional scratch fs mechanics (extracted leaf)
#include "xi_project_model.hpp" // ProjectInfo / InstanceInfo / CompileEnv / OpenWarning (data model)
#include "xi_script_compiler.hpp"
#include "xi_trigger_bus.hpp"

#include "yyjson.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <utility>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xi {

// (pm_json_escape moved to xi_pm_json.hpp)
// (plugin_abi_compatible / PluginInfo / CAbiInstanceAdapter moved to
//  xi_cabi_adapter.hpp; InstanceInfo / ProjectInfo / CompileEnv to
//  xi_project_model.hpp — both included above.)

class PluginManager {
public:
    void set_compile_env(const CompileEnv& env) {
        std::lock_guard<std::mutex> lk(mu_);
        compile_env_ = env;
    }

    ~PluginManager() {
        // Destroy instances FIRST, while their plugin DLLs are still mapped:
        // ~CAbiInstanceAdapter calls the plugin's destroy_fn (a code pointer INTO the
        // DLL), so a FreeLibrary before that is a call into unmapped memory — an
        // access violation on what should be a clean exit (the still-armed crash
        // filter then fabricates a spurious minidump). close_project() already does
        // this order at runtime; this is the static-destruction / never-closed-project
        // BACKSTOP (controlled_shutdown_teardown_ now calls close_project for the
        // normal path, but an exit that skips it must still not invert the order).
        // The adapter's ImagePool sweep is itself guarded by g_image_pool_alive for
        // the case the pool singleton was already torn down before us.
        project_.instances.clear();
        inst_state_.clear();
        // V3: machine-autoloaded providers are NOT in project_.instances (they
        // outlive projects), so tear them down here too — BEFORE the FreeLibrary
        // loop below, for the exact same dangling-destroy_fn reason. Their DLLs
        // are global (scanned, not project-loaded), so they're in plugins_ and
        // get freed by the loop after their adapters are gone. Drop the
        // InstanceRegistry ref FIRST (it holds a shared_ptr too) so clearing the
        // map actually destroys the adapter here — the same discipline
        // close_project applies to project instances.
        for (auto& [pname, inst] : machine_instances_)
            if (inst) InstanceRegistry::instance().remove(inst->name());
        machine_instances_.clear();
        // Now release every loaded plugin DLL — no live destroy_fn callers remain.
        for (auto& [name, pi] : plugins_) {
            if (pi.handle) {
                FreeLibrary(pi.handle);
                pi.handle = nullptr;
            }
        }
    }

    // DESTRUCTIVE-METHOD CONVENTION: every method that mutates/frees adapters,
    // instances, or plugin DLLs (which dispatch workers could still be calling
    // into) takes a `const QuiesceToken&` as its FIRST parameter — proof the
    // caller has quiesced dispatch. Only DispatchPoolGuard (via
    // quiesce_dispatch_for_lifecycle_op_) or an explicit
    // QuiesceToken::assert_no_dispatch() can mint one — forgetting the quiesce
    // is a compile error. See xi_quiesce_token.hpp.

    // ---- plugin discovery / certification / registration (xi_pm_discovery.hpp) ----
    // DESTRUCTIVE: the "moved" branch FreeLibrary's a plugin DLL a live adapter
    // may still hold (RT5/J2).
    int  scan_plugins(const QuiesceToken& quiesced, const std::string& plugins_dir);
    void set_certify_exe(const std::string& exe);
    std::vector<OpenWarning> certify_warnings();
    bool unquarantine_plugin(const std::string& name_or_folder);
    bool register_plugin_folder_locked_(const std::string& folder,
                                        bool track_as_project = false);
    static std::string expand_plugin_root_(std::string r, const std::string& project_folder);
    void resolve_external_project_plugins_locked_(
            const std::string& project_folder,
            const std::vector<std::string>& dirs_raw,
            const std::vector<ProjectInfo::PluginRef>& refs);
    int  compile_project_plugins(const std::string& project_folder);

    // ---- DLL load / compile / recompile / rebuild / export (xi_pm_load.hpp) ----
    bool is_project_plugin(const std::string& name);
    bool load_plugin(const std::string& name, std::string* err = nullptr);
    std::vector<PluginInfo> list_plugins();
    PluginInfo* find_plugin(const std::string& name);
    bool plugin_location(const std::string& name, std::string& folder, std::string& dll);

    // ---- project management + working copy (xi_pm_project.hpp) ----
    bool create_project(const std::string& folder, const std::string& name);
    // DESTRUCTIVE: destroys every instance adapter + FreeLibrary's the
    // project's plugin DLLs (P0-AB-3).
    void close_project(const QuiesceToken& quiesced);
    // Constants + the filesystem mechanics (seed/mirror/exclude/gitignore) live
    // in xi_working_copy.hpp; these aliases keep the references terse. The
    // stateful transactional methods (open/commit/discard) stay members.
    static constexpr const char* kWorkingCopyDir = xi::wc::kWorkingCopyDir;
    static constexpr const char* kCommitMarker   = xi::wc::kCommitMarker;
    // The canonical project dir when a working copy is active; empty otherwise.
    const std::string& canonical_path() const { return canonical_path_; }
    bool has_working_copy() const { return !canonical_path_.empty(); }
    // DESTRUCTIVE: the commit mirrors (add/overwrite/delete) the scratch that
    // continuous workers read/write.
    bool commit_working_copy(const QuiesceToken& quiesced);
    // DESTRUCTIVE: close + reopen (both destructive; token threads through).
    bool reopen_fresh_working_copy(const QuiesceToken& quiesced);
    // DESTRUCTIVE: tears down the previous project's instances and
    // FreeLibrary's its plugin DLLs before loading the new one (P0-AB-3).
    bool open_project(const QuiesceToken& quiesced,
                      const std::string& folder_arg, bool working_copy = false);

    // ---- instance CRUD / lifecycle state / persistence (xi_pm_instances.hpp) ----
    static bool is_valid_instance_name(const std::string& n);
    // DESTRUCTIVE: can EVICT a live machine-autoload provider of the same
    // plugin (adapter dtor + cap/ref/pack owner sweeps) — RT5/J3.
    InstanceInfo* create_instance(const QuiesceToken& quiesced,
                                  const std::string& instance_name,
                                  const std::string& plugin_name,
                                  std::string* err = nullptr);
    bool save_instance(const std::string& instance_name);
    // DESTRUCTIVE: destroys a DLL-backed runtime adapter (finding 5).
    bool remove_instance(const QuiesceToken& quiesced,
                         const std::string& instance_name, bool delete_folder);
    // Result of rename_instance. The caller MUST distinguish Rejected (no mutation
    // happened — the old instance is untouched) from NotPersisted (the runtime +
    // on-disk folder were renamed to new_name, only the config save failed): on
    // NotPersisted the in-memory state IS the new name, so the caller still has to
    // migrate any side state keyed by name (e.g. g_inst_state) and report a
    // save-failed warning, NOT "rename failed" — reporting failure while the
    // runtime moved would desync name-keyed state.
    enum class RenameResult { Rejected, Ok, NotPersisted };
    // DESTRUCTIVE: destroys the OLD adapter in place to rebuild it under the
    // new name (L1 — the last un-quiesced live-adapter teardown).
    RenameResult rename_instance(const QuiesceToken& quiesced,
                                 const std::string& old_name, const std::string& new_name);

    ProjectInfo& project() { return project_; }

    std::string instance_group(const std::string& name);

    // A locked, read-only snapshot of the live instance set for observers (the
    // health + image-pool-stats readers) that must NOT iterate project().instances
    // unlocked — an unlocked read races create/remove/rename_instance's map
    // mutations (erase() → dangling entry / UAF), the exact race instance_group()
    // guards mu_ against. Iterate the returned vector instead of the raw map. The
    // shared_ptr keeps each instance alive for the whole read even if a concurrent
    // remove erases it from the map. Deliberately a COPIED snapshot (not a locked
    // visitor callback): the health reader re-enters get_instance_state(), which
    // takes mu_ — running that under this lock would self-deadlock. Small fields
    // only; take the lock, copy, release.
    struct InstanceSnapshot {
        std::string                   name;         // the map key = instance identity
        std::string                   plugin_name;
        std::shared_ptr<InstanceBase> instance;     // held alive for the reader
    };
    std::vector<InstanceSnapshot> snapshot_instances();

    // ---- V3 machine-level lib-plugin autoload (doc 14 / doc 19 V3) ----------
    // Instantiate every discovered plugin marked `"autoload": true` ONCE under a
    // stable machine owner, so its capabilities register WITHOUT any project
    // declaring a per-instance (cures E1's second cause, doc 06 §6). Idempotent
    // + a reconciler: it creates a machine provider only for an autoload plugin
    // that has NO machine instance yet AND no project instance (a project
    // instance takes precedence). Called at service boot (after scan_plugins) and
    // again after project teardown (close_project / remove_instance) to reinstate
    // providers a closed project had displaced. Returns the count created.
    // (definition in xi_pm_load.hpp)
    int autoload_machine_providers();
    // Deployment opt-in for machine autoload. OFF by default: a stock deployment
    // is byte-unchanged (no machine providers, so nothing that keys off a
    // capability's availability — e.g. expose's E2 JPEG preview — flips). The
    // service sets this from `--autoload-lib` / env XINSP2_AUTOLOAD_LIB BEFORE
    // the boot autoload pass. While OFF, every autoload path is a no-op, so the
    // reconcilers wired into open/close/create/remove stay inert too.
    void set_autoload_enabled(bool on) {
        std::lock_guard<std::mutex> lk(mu_);
        autoload_enabled_ = on;
    }
    bool autoload_enabled() {
        std::lock_guard<std::mutex> lk(mu_);
        return autoload_enabled_;
    }
    // Machine-scoped recovery: rebuild a machine provider from a fresh factory
    // (the analogue of an operator re-committing a project instance's config to
    // clear a quarantine). Evicts the current machine adapter for `plugin_name`
    // and re-autoloads it. Returns true if a provider is live afterwards.
    // (definition in xi_pm_load.hpp)
    bool reload_machine_provider(const std::string& plugin_name);
    // Test/inspection: names of the plugins currently machine-provided.
    std::vector<std::string> machine_provider_plugins();

    // ---- host-tracked instance lifecycle state -----------------------------
    // The state map is OWNED here, under the same mu_ as the instance set, so
    // create/remove/rename migrate it atomically (they hold mu_ and call
    // set_state_locked_ / erase / re-key inline). Keyed by name to span backend +
    // script-side instances alike. set_*/get_* are the public (locking) entry
    // points used by the WS handlers; CRUD methods use set_state_locked_ while
    // holding mu_.
    void set_instance_state(const std::string& name, InstState s,
                            const std::string& err = std::string());
    bool get_instance_state(const std::string& name, InstState& out_state,
                            std::string& out_err, long long* out_crash_count = nullptr);
    void note_instance_crash(const std::string& name, const std::string& why);
    void clear_instance_states();

    std::string to_json();

    // Per-instance failure record from the most recent open_project: the open
    // succeeds even if individual instances fail (skip-bad-instance); callers
    // read these to surface a warning. OpenWarning lives in xi_project_model.hpp
    // (namespace xi) — used unqualified here, resolves to xi::OpenWarning.
    std::vector<OpenWarning> open_warnings();

    // A hard, whole-project open refusal — distinct from the per-instance
    // skip-bad warnings above (those still open the project). Currently set only
    // when project.json declares an unrecognized FUTURE schema major (the
    // project-file analogue of the plugin ABI gate refusing a too-new plugin).
    // Empty when the last open_project did not hard-refuse; cleared at the start
    // of every open_project so a stale reason never leaks into a later open.
    std::string open_error();

private:
    // Part III G1.1/G1.2 — certify a plugin folder, cached by DLL content hash.
    // (defined in xi_pm_discovery.hpp)
    certify::Verdict certify_folder_locked_(const std::string& folder,
                                            const PluginInfo& info);

    // Off-lock cache-warm for certify_folder_locked_ — runs the throwaway-child
    // certify subprocess (up to 30s) when the DLL hash is missing/changed and
    // writes the verdict to the on-disk cache WITHOUT holding mu_. scan_plugins
    // calls this in an UNLOCKED pre-pass so the subsequent locked certify_folder_
    // locked_ reads the fresh cache instead of spawning under mu_ (which would
    // stall instance_group() on the source emit hot path for up to 30s/plugin).
    // Touches only the passed exe snapshot + the on-disk cache — no PluginManager
    // shared state — so it is data-race free off-lock. (defined in xi_pm_discovery.hpp)
    static void precertify_folder_(const std::string& folder, const PluginInfo& info,
                                   const std::string& certify_exe);

    // Record the on-disk write-time of the DLL we just loaded, so
    // reload_changed_plugins() can later tell whether a rebuild produced a new
    // DLL (and only hot-swap the ones that actually moved).
    static void stamp_loaded_dll_(PluginInfo& pi, const std::string& dll_path) {
        std::error_code ec;
        auto wt = std::filesystem::last_write_time(dll_path, ec);
        pi.loaded_dll_mtime = ec ? 0 : (uint64_t)wt.time_since_epoch().count();
    }

    // Re-read the manifest flags that drive DISPATCH semantics — reentrant
    // (alias thread_safe), is_sink, json_fallback — plus the factory symbol, from
    // already-loaded plugin.json text, and apply them onto `pi`. The single source
    // of truth shared by EVERY (re)load path (full compile, hot recompile, cmake
    // reattach) so toggling these in plugin.json + Save/Rebuild actually changes
    // behaviour: a now-non-reentrant plugin starts serializing (effective_cap_→1),
    // a newly-`sink` plugin starts landing in frame order. Uses the #22-hardened
    // top-level-only flag/string parse. Cold path (load) — clarity over micro-opt.
    static void apply_manifest_flags_from_content_(PluginInfo& pi, const std::string& mc) {
        if (auto f = extract_string(mc, "factory")) pi.factory_symbol = *f;
        pi.reentrant     = json_flag_true(mc, "reentrant") ||
                           json_flag_true(mc, "thread_safe");   // documented alias
        pi.json_fallback = json_flag_true(mc, "json_fallback");
        pi.is_sink       = json_flag_true(mc, "sink") ||
                           (extract_string(mc, "role").value_or("") == "sink");
        // Keep the doc-14 lib marker + V3 autoload gate refreshed on reload too,
        // so toggling them in plugin.json + Rebuild takes effect (parse_manifest
        // is the scan-path twin of this reload-path source of truth).
        pi.is_lib        = json_flag_true(mc, "lib");
        pi.autoload      = json_flag_true(mc, "autoload");
        // item 14: per-plugin post-fault policy DEFAULT (an instance.json
        // "on_fault" overrides it). Unknown/absent → Reuse (today's behavior).
        pi.default_on_fault = parse_on_fault(extract_string(mc, "on_fault").value_or(""),
                                             OnFault::Reuse);
        // Refresh the LV2-style capability handshake too, so a rebuilt plugin that
        // newly declares (or drops) a required interface is re-gated on reload.
        pi.required_ifaces = parse_iface_reqs(mc, "requires");
        pi.optional_ifaces = parse_iface_reqs(mc, "optional");
    }
    // Convenience: read <folder>/plugin.json and apply. No-op if absent (keeps the
    // pi's current flags). `folder` = the plugin's SOURCE folder (where plugin.json
    // lives), NOT its build/ DLL dir.
    static void apply_manifest_flags_(PluginInfo& pi, const std::string& folder) {
        auto mpath = std::filesystem::path(folder) / "plugin.json";
        if (!std::filesystem::exists(mpath)) return;
        std::ifstream mf(mpath.string());
        std::stringstream ms; ms << mf.rdbuf();
        apply_manifest_flags_from_content_(pi, ms.str());
    }

    // The one DLL-load primitive every load site routes through, so a plugin
    // resolves its sidecar dependency DLLs the same way no matter how it was
    // discovered (project compile/recompile/reattach vs. global/auto-load).
    // LoadLibraryEx with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR searches the plugin's
    // OWN folder for its dependency DLLs — a plugin can ship extra .dll deps
    // right next to its plugin DLL. DEFAULT_DIRS keeps the app dir (where
    // OpenCV/turbojpeg/IPP are deployed) + System32 + AddDllDirectory dirs in
    // the search set; it deliberately drops CWD/PATH (avoids accidental hijack).
    // NOTE: same-named DLLs still collide across plugins — Windows keeps one
    // module per base name per process (see docs/guides/write-a-plugin.md).
    // TODO(linux): dlopen resolves deps via RPATH/$ORIGIN + LD_LIBRARY_PATH;
    // build plugin .so with -Wl,-rpath,$ORIGIN for the same "deps beside me".
    static HMODULE load_plugin_dll_(const std::string& path) {
        return LoadLibraryExA(path.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    }

    // Host-side cmake invocation (newest_source_mtime / run_cmd_capture /
    // build_cmake_plugin) moved to xi_cmake_build.hpp (xi::cmake_build::;
    // included above). rebuild_cmake_plugins below orchestrates them.

    // One instance preserved across a plugin reload: its name + folder +
    // serialized def, captured before destruction and replayed after reload.
    struct PendingInstance {
        std::string name, folder, def_json;
        int         max_concurrency = 0;
        OnFault     on_fault = OnFault::Reuse;   // item 14: preserved across reload
    };

    // Owner-guarded factory call shared by every (re)instantiation path on the
    // reload/recompile lanes. (defined in xi_pm_load.hpp)
    std::shared_ptr<CAbiInstanceAdapter> make_adapter_guarded_(
            PluginInfo& pi, const std::string& plugin_name,
            const std::string& inst_name, int max_concurrency,
            OnFault on_fault = OnFault::Reuse);

    // V3 autoload internals — mu_ MUST be held (definitions in xi_pm_load.hpp).
    // load_plugin's body factored so autoload can LoadLibrary while holding mu_.
    bool load_plugin_locked_(const std::string& name, std::string* err);
    // The reconciler body (autoload_machine_providers wraps it under the lock).
    int  autoload_machine_providers_locked_();
    // Destroy the machine provider for `plugin_name` (adapter dtor owner-sweeps
    // its capability registrations); no-op if none. Called BEFORE a project
    // instance of that plugin runs its factory, so the project instance registers
    // the slot cleanly (project precedence, no double-register).
    void evict_machine_provider_locked_(const std::string& plugin_name);
    // True if some project instance currently uses `plugin_name`.
    bool project_provides_plugin_locked_(const std::string& plugin_name) const;

    // Phase 1 of a reload: snapshot every instance's def, then destroy them and
    // FreeLibrary the plugin's old DLL. (defined in xi_pm_load.hpp)
    std::vector<PendingInstance> detach_plugin_instances_locked_(
            const std::string& plugin_name, HMODULE* old_base_out);

    // Phase 2 of a reload: load the (re)built DLL, re-resolve the factory, and
    // re-instantiate every snapshotted instance. (defined in xi_pm_load.hpp)
    bool reattach_plugin_from_dll_locked_(const std::string& plugin_name,
                                          const std::string& new_dll_path,
                                          const std::vector<PendingInstance>& pending,
                                          HMODULE old_base,
                                          std::string* err);

    int compile_project_plugins_locked(const std::string& project_folder);
    int compile_plugin_folders_locked_(const std::vector<std::filesystem::directory_entry>& folders);

public:
    // Hot-rebuild one project plugin and re-instantiate every instance
    // using it, preserving each instance's saved def. Used by the
    // extension's file watcher. (defined in xi_pm_load.hpp)
    struct RecompileResult {
        bool                     ok = false;
        std::string              build_log;
        std::vector<xi::script::Diagnostic> diagnostics;
        std::vector<std::string> reattached_instances;
        std::string              error;
    };
    // DESTRUCTIVE: resets each instance adapter then FreeLibrary's the old DLL
    // (P0-AB-4).
    RecompileResult recompile_project_plugin(const QuiesceToken& quiesced,
                                             const std::string& plugin_name);

    struct PluginRebuildReport {
        // status: "rebuilt" | "unchanged" | "failed"
        struct Item { std::string name, status, detail; };
        std::vector<Item> items;
    };
    // DESTRUCTIVE: unload → cmake build → reload per changed plugin.
    PluginRebuildReport rebuild_cmake_plugins(const QuiesceToken& quiesced,
                                              const std::string& cmake_exe,
                                              const std::string& config,
                                              const std::vector<std::string>& only = {});

    using ExportResult = xi::PluginExportResult;
    // DESTRUCTIVE-adjacent: recompiles in Release while workers could be
    // mid-call into the same plugin's instances (quiesced like its siblings).
    ExportResult export_project_plugin(const QuiesceToken& quiesced,
                                        const std::string& plugin_name,
                                        const std::string& dest_root);

private:
    // mu_ MUST be held. Set/overwrite the lifecycle state for `name`; last_error
    // is kept only for the Faulted state (cleared otherwise), matching the old
    // free-function semantics in service_main.
    void set_state_locked_(const std::string& name, InstState s, const std::string& err) {
        auto& r = inst_state_[name];
        r.state = s;
        r.last_error = (s == InstState::Faulted) ? err : std::string();
    }

    std::mutex mu_;
    std::unordered_map<std::string, PluginInfo> plugins_;
    // V3: machine-autoloaded lib providers, keyed by PLUGIN name (one machine
    // instance per autoload plugin). Distinct from project_.instances — these are
    // machine-scoped: created at boot, NOT torn down by close_project, swept in
    // ~PluginManager. Each holds a CAbiInstanceAdapter also registered in
    // InstanceRegistry (so the capability funnel's resolve_provider_ finds it).
    std::unordered_map<std::string, std::shared_ptr<CAbiInstanceAdapter>> machine_instances_;
    // V3 deployment opt-in (see set_autoload_enabled). OFF = no autoload anywhere.
    bool autoload_enabled_ = false;
    ProjectInfo project_;
    // Host-tracked instance lifecycle state (see the public set/get above). Guarded
    // by mu_; migrated inline by create/remove/rename so it never drifts.
    std::unordered_map<std::string, InstStateRec> inst_state_;
    std::vector<OpenWarning> last_open_warnings_;
    // Hard whole-project open refusal reason (see open_error()). Empty unless the
    // last open_project refused the file outright (unrecognized future schema).
    std::string last_open_error_;
    // Part III G1 — path to the binary that handles `--certify-plugin <dir>`
    // (self, in practice). Empty disables spawning new certifications; cached
    // verdicts on disk are still honoured. Plugins skipped this scan because
    // their cached verdict was `crashed`.
    std::string certify_exe_;
    std::vector<OpenWarning> last_certify_warnings_;
    // Part III G2.3 — name -> folder for plugins gated out at the last scan
    // (crashed or quarantined), so cmd:unquarantine_plugin can find the folder to
    // clear even though the plugin was never registered into plugins_.
    std::unordered_map<std::string, std::string> gated_folders_;
    // BUG#4 quarantine: project.json existed but failed to parse on the last
    // open_project. While true, save_project_locked REFUSES the full-rebuild
    // write (it would clobber extension-owned keys — params / auto_respawn /
    // watchdog_ms — and destroy the recoverable original bytes). The project
    // stays open in a degraded/read-only-ish mode (like a compile failure) and
    // the corrupt bytes are preserved as project.json.corrupt-<ts>. Reset to
    // false only by a *fresh successful open* of a well-formed project.json —
    // not by a save (saves are blocked while this is set).
    bool project_json_malformed_ = false;
    CompileEnv  compile_env_;
    // Names of plugins that came from <project>/plugins/ rather than the
    // global plugins directory — flagged so the UI can label them and so
    // we don't re-scan their dll mtime against the global cert.
    std::unordered_map<std::string, std::string> project_plugin_origin_;
    // #9/#13: every plugin THIS open project caused to load — compiled project
    // plugins (a superset of project_plugin_origin_'s keys) AND compile:false
    // externals resolved from plugin_dirs — keyed by the SAME key that holds the
    // HMODULE in plugins_ (the manifest name, which can differ from the folder
    // leaf). close/open frees + erases every one by this key, so no project leaks an
    // HMODULE or leaves a stale entry shadowing the next project's same-named plugin.
    std::unordered_set<std::string> project_loaded_plugins_;
    // Canonical project dir when a working copy is active (project_.folder_path
    // then points at <canonical>/.xinsp_work). Empty = no working copy.
    std::string canonical_path_;

    // Shared host_api for in-process C-ABI plugin factory calls (image-pool
    // host). One process-wide instance: every factory site used to declare its
    // own byte-identical function-local static — this dedups them. Cold path
    // (instance create / recompile / rename), so the single shared static is fine
    // and costs nothing extra.
    static xi_host_api& default_host_api() {
        static xi_host_api host = []{
            auto a = ImagePool::make_host_api();
            // THE CUT: the Record emit hook (install_trigger_hook) is gone —
            // sources emit packs through the pack ABI wired below.
            // polaris2 wave-2: publish the xi.pack@1 data plane + wire the pack
            // dispatch releaser, so a pack-capable plugin resolving the door off
            // this table gets a live interface (and emit_pack reaches the bus).
            install_pack_abi();
            // Capability plane pilot (docs/new_gen/14): publish xi.cap@1 +
            // xi.cap.provider@1 and wire the registration owner-sweeper, so a
            // lib plugin registering in its factory (which runs under this
            // table) reaches a live registration door.
            install_cap_plane();
            // DEBUG freeze-guard: this is the FULLY WIRED table plugins receive, so
            // every carved get_interface entry must track its struct-field twin
            // (emit_record via the published slot). Catches door/field drift at
            // startup so the xi.emit@1-null landmine can never silently return.
            assert(ImagePool::door_matches_fields(a) &&
                   "carved interface door drifted from its xi_host_api struct fields");
            return a;
        }();
        return host;
    }

    // Collect .cpp/.cc/.cxx source files directly under `dir` (non-recursive)
    // into `out`. Dedups the identical source-walk lambda that lived in
    // compile / recompile / export. Cold path (compile time).
    static void collect_cpp_sources(const std::filesystem::path& dir,
                                    std::vector<std::string>& out) {
        for (auto& f : std::filesystem::directory_iterator(dir)) {
            if (!f.is_regular_file()) continue;
            auto ext = f.path().extension().string();
            if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") out.push_back(f.path().string());
        }
    }

    // Working-copy filesystem mechanics (wc_excluded / copy_tree_excluding /
    // mirror_tree / ensure_gitignore) moved to xi_working_copy.hpp (xi::wc::;
    // included above). The transactional methods call into them.

    // json_flag_true / extract_string / detail_find_key / parse_manifest moved
    // to xi_pm_parse.hpp; validate_config_against_manifest to xi_config_validate.hpp
    // (leaf; both included above). Called unqualified -> resolve in namespace xi.

    // Carry over any top-level project.json keys this writer doesn't manage.
    // (defined in xi_pm_instances.hpp)
    static std::string merge_unknown_top_keys_(const std::string& fresh_json,
                                               const std::filesystem::path& existing);

    bool save_project_locked();
    bool save_instance_json(const InstanceInfo& ii);
};

} // namespace xi
