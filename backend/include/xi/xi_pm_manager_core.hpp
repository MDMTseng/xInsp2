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
#include "xi_atomic_io.hpp"
#include "xi_clock.hpp"        // wall_ms() — portable timestamp for .corrupt-<ts> quarantine names
#include "xi_cabi_adapter.hpp" // plugin_abi_compatible / PluginInfo / CAbiInstanceAdapter
#include "xi_certify.hpp"     // Part III G1: scan/certification isolation (child-process certify + verdict cache)
#include "xi_image_pool.hpp"
#include "xi_instance.hpp"
#include "xi_config_validate.hpp" // validate_config_against_manifest (opt-in diagnostic, extracted leaf)
#include "xi_pm_json.hpp"      // pm_json_escape / pm_json_quote (extracted leaf)
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

// (pm_json_escape / pm_json_quote moved to xi_pm_json.hpp)
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
        // Now release every loaded plugin DLL — no live destroy_fn callers remain.
        for (auto& [name, pi] : plugins_) {
            if (pi.handle) {
                FreeLibrary(pi.handle);
                pi.handle = nullptr;
            }
        }
    }

    // ---- plugin discovery / certification / registration (xi_pm_discovery.hpp) ----
    int  scan_plugins(const std::string& plugins_dir);
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
    void close_project();
    // Constants + the filesystem mechanics (seed/mirror/exclude/gitignore) live
    // in xi_working_copy.hpp; these aliases keep the references terse. The
    // stateful transactional methods (open/commit/discard) stay members.
    static constexpr const char* kWorkingCopyDir = xi::wc::kWorkingCopyDir;
    static constexpr const char* kCommitMarker   = xi::wc::kCommitMarker;
    // The canonical project dir when a working copy is active; empty otherwise.
    const std::string& canonical_path() const { return canonical_path_; }
    bool has_working_copy() const { return !canonical_path_.empty(); }
    bool commit_working_copy();
    bool reopen_fresh_working_copy();
    bool open_project(const std::string& folder_arg, bool working_copy = false);

    // ---- instance CRUD / lifecycle state / persistence (xi_pm_instances.hpp) ----
    static bool is_valid_instance_name(const std::string& n);
    InstanceInfo* create_instance(const std::string& instance_name,
                                  const std::string& plugin_name,
                                  std::string* err = nullptr);
    bool save_instance(const std::string& instance_name);
    bool remove_instance(const std::string& instance_name, bool delete_folder);
    // Result of rename_instance. The caller MUST distinguish Rejected (no mutation
    // happened — the old instance is untouched) from NotPersisted (the runtime +
    // on-disk folder were renamed to new_name, only the config save failed): on
    // NotPersisted the in-memory state IS the new name, so the caller still has to
    // migrate any side state keyed by name (e.g. g_inst_state) and report a
    // save-failed warning, NOT "rename failed" — reporting failure while the
    // runtime moved would desync name-keyed state.
    enum class RenameResult { Rejected, Ok, NotPersisted };
    RenameResult rename_instance(const std::string& old_name, const std::string& new_name);

    ProjectInfo& project() { return project_; }

    std::string instance_group(const std::string& name);

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
    // module per base name per process (see adding-a-plugin.md).
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
    RecompileResult recompile_project_plugin(const std::string& plugin_name);

    struct PluginRebuildReport {
        // status: "rebuilt" | "unchanged" | "failed"
        struct Item { std::string name, status, detail; };
        std::vector<Item> items;
    };
    PluginRebuildReport rebuild_cmake_plugins(const std::string& cmake_exe,
                                              const std::string& config,
                                              const std::vector<std::string>& only = {});

    using ExportResult = xi::PluginExportResult;
    ExportResult export_project_plugin(const std::string& plugin_name,
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

    // Shared host_api for in-process C-ABI plugin factory calls (image-pool host
    // + trigger hook). One process-wide instance: every factory site used to
    // declare its own byte-identical function-local static — this dedups them.
    // Cold path (instance create / recompile / rename), so the single shared
    // static is fine and costs nothing extra.
    static xi_host_api& default_host_api() {
        static xi_host_api host = []{
            auto a = ImagePool::make_host_api();
            install_trigger_hook(a);
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
