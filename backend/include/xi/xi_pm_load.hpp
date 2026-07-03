#pragma once
//
// xi_pm_load.hpp — PluginManager DLL load / compile / hot-recompile / cmake
// rebuild / export concern.
//
// Out-of-class inline definitions for:
//   make_adapter_guarded_ / detach_plugin_instances_locked_ /
//   reattach_plugin_from_dll_locked_ / compile_project_plugins_locked /
//   compile_plugin_folders_locked_ / recompile_project_plugin /
//   rebuild_cmake_plugins / export_project_plugin / is_project_plugin /
//   load_plugin / list_plugins / find_plugin / plugin_location
//
// Mechanically extracted from xi_plugin_manager.hpp — bodies are byte-identical
// to the former in-class definitions.
//
#include "xi_pm_manager_core.hpp"

#include <algorithm>          // std::min/std::max (rebuild) — via core, kept explicit
#include <cstdio>
#include <filesystem>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace xi {

// Owner-guarded factory call shared by every (re)instantiation path on the
// reload/recompile lanes. Pre-allocates an ImagePool owner id and wraps the
// ctor in an OwnerGuard so images the ctor allocates are tagged, then either
// adopted by the adapter (success) or swept (throw/null). create_instance and
// project-load do the same inline; the reload paths used to skip it, so a
// plugin allocating pool images in its ctor leaked them on every hot-reload.
inline std::shared_ptr<CAbiInstanceAdapter> PluginManager::make_adapter_guarded_(
        PluginInfo& pi, const std::string& plugin_name,
        const std::string& inst_name, int max_concurrency, OnFault on_fault) {
    if (!pi.c_factory) return nullptr;
    xi_host_api& host = default_host_api();
    ImagePoolOwnerScope owner;   // sweeps the ctor's images unless adopted below
    void* raw = nullptr;
    try {
        raw = owner.run_factory([&] { return pi.c_factory(&host, inst_name.c_str()); });
    } catch (...) { raw = nullptr; }
    if (!raw) return nullptr;    // owner dtor sweeps
    auto inst = std::make_shared<CAbiInstanceAdapter>(
        inst_name, plugin_name, pi.handle, raw, pi.reentrant, max_concurrency, pi.is_sink);
    inst->adopt_owner_id(owner.release());
    inst->set_on_fault(on_fault);            // item 14: carry the per-instance policy
    inst->arm_reinit(pi.c_factory, &host);   // enable in-place reinit on this DLL
    return inst;
}

// ---- V3 machine-level lib-plugin autoload (doc 14 / doc 19 V3) --------------
inline bool PluginManager::project_provides_plugin_locked_(const std::string& plugin_name) const {
    for (auto& [iname, ii] : project_.instances)
        if (ii.plugin_name == plugin_name) return true;
    return false;
}

inline void PluginManager::evict_machine_provider_locked_(const std::string& plugin_name) {
    auto it = machine_instances_.find(plugin_name);
    if (it == machine_instances_.end()) return;
    // Drop it from the live-instance registry FIRST (so the funnel's
    // resolve_provider_ can't hand it out mid-teardown), then release our ref.
    // The adapter dtor runs the plugin destroy_fn AND owner-sweeps its capability
    // registrations (CAbiInstanceAdapter::~ -> sweep_caps_for), freeing the names
    // for the incoming project instance to claim.
    InstanceRegistry::instance().remove(it->second->name());
    machine_instances_.erase(it);   // shared_ptr drop -> ~CAbiInstanceAdapter
}

inline int PluginManager::autoload_machine_providers_locked_() {
    if (!autoload_enabled_) return 0;   // deployment opt-in gate (V3)
    int created = 0;
    for (auto& [name, pi] : plugins_) {
        if (!pi.autoload) continue;                 // not an autoload lib plugin
        if (machine_instances_.count(name)) continue;   // already machine-provided
        if (project_provides_plugin_locked_(name)) continue;  // project wins
        std::string err;
        if (!load_plugin_locked_(name, &err)) {     // no-op if already loaded
            std::fprintf(stderr, "[xinsp2] autoload: cannot load lib plugin '%s': %s\n",
                         name.c_str(), err.c_str());
            continue;
        }
        auto pit = plugins_.find(name);
        if (pit == plugins_.end() || !pit->second.c_factory) continue;
        // Machine-owned instance: synthetic name (not a valid user instance name,
        // so it never collides with a project instance), fresh owner id, the
        // plugin's declared on_fault policy. NOT added to project_.instances.
        const std::string inst_name = "@auto:" + name;
        auto inst = make_adapter_guarded_(pit->second, name, inst_name,
                                          /*max_concurrency=*/0, pit->second.default_on_fault);
        if (!inst) {
            std::fprintf(stderr, "[xinsp2] autoload: factory failed for lib plugin '%s' "
                         "(it registered no capabilities)\n", name.c_str());
            continue;
        }
        InstanceRegistry::instance().add(inst);
        machine_instances_[name] = inst;
        ++created;
        std::fprintf(stderr, "[xinsp2] autoload: machine lib provider '%s' up (owner=%llu)\n",
                     name.c_str(), (unsigned long long)inst->owner_id());
    }
    return created;
}

inline int PluginManager::autoload_machine_providers() {
    std::lock_guard<std::mutex> lk(mu_);
    return autoload_machine_providers_locked_();
}

inline bool PluginManager::reload_machine_provider(const std::string& plugin_name) {
    std::lock_guard<std::mutex> lk(mu_);
    // Only meaningful for an autoload plugin with no project instance holding the
    // slot; if a project instance provides it, that one is authoritative.
    if (project_provides_plugin_locked_(plugin_name)) return false;
    evict_machine_provider_locked_(plugin_name);   // drop the quarantined/stale one
    autoload_machine_providers_locked_();           // fresh factory re-registers
    return machine_instances_.count(plugin_name) > 0;
}

inline std::vector<std::string> PluginManager::machine_provider_plugins() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.reserve(machine_instances_.size());
    for (auto& [name, inst] : machine_instances_) out.push_back(name);
    return out;
}

// Phase 1 of a reload: snapshot every instance's def, then destroy them and
// FreeLibrary the plugin's old DLL. The instance dtor runs xi_plugin_destroy
// (joins worker threads / frees a CUDA context) BEFORE we unload, and freeing
// the DLL releases the on-disk file lock so a rebuild (cl.exe-versioned or
// cmake fixed-name) can overwrite it. Returns the snapshots + the old module
// base (for the post-reload stale-module fail-check). mu_ MUST be held.
inline std::vector<PluginManager::PendingInstance> PluginManager::detach_plugin_instances_locked_(
        const std::string& plugin_name, HMODULE* old_base_out) {
    std::vector<PendingInstance> pending;
    for (auto& [iname, ii] : project_.instances) {
        if (ii.plugin_name != plugin_name) continue;
        PendingInstance p; p.name = iname; p.folder = ii.folder_path;
        p.max_concurrency = ii.max_concurrency;
        p.on_fault = ii.on_fault;   // item 14: preserve policy across the reload
        if (ii.instance) p.def_json = ii.instance->get_def();
        pending.push_back(std::move(p));
    }
    for (auto& p : pending) {
        auto& ii = project_.instances[p.name];
        InstanceRegistry::instance().remove(p.name);
        ii.instance.reset();   // dtor -> xi_plugin_destroy
    }
    auto pi_it = plugins_.find(plugin_name);
    HMODULE base = (pi_it != plugins_.end()) ? pi_it->second.handle : nullptr;
    if (old_base_out) *old_base_out = base;
    if (pi_it != plugins_.end() && pi_it->second.handle) {
        FreeLibrary(pi_it->second.handle);
        pi_it->second.handle = nullptr;
        pi_it->second.c_factory = nullptr;
    }
    return pending;
}

// Phase 2 of a reload: load the (re)built DLL, re-resolve the factory, and
// re-instantiate every snapshotted instance with its def restored. Fail-check:
// a module that maps at the SAME base as before means FreeLibrary didn't
// actually unload it (a lingering worker thread or a pinning dependency DLL)
// — the OLD code is still live. We re-attach instances against it (so nothing
// dangles) but report failure loudly, since silently running stale code is the
// exact bug to surface. mu_ MUST be held. Returns false + *err on failure.
inline bool PluginManager::reattach_plugin_from_dll_locked_(const std::string& plugin_name,
                                      const std::string& new_dll_path,
                                      const std::vector<PendingInstance>& pending,
                                      HMODULE old_base,
                                      std::string* err) {
    auto pi_it = plugins_.find(plugin_name);
    if (pi_it == plugins_.end()) {
        if (err) *err = "plugin not registered: " + plugin_name;
        return false;
    }
    auto& pi = pi_it->second;
    // #23: re-read the dispatch-flag manifest (reentrant/is_sink/json_fallback/
    // factory) from plugin.json BEFORE rebuilding adapters, so a cmake rebuild
    // after toggling those flags applies the NEW semantics — the prior code
    // reused the stale `pi` fields, so a now-non-reentrant plugin kept running
    // unserialized. Source folder = the plugin's origin (where plugin.json
    // lives), falling back to the DLL's folder for a standalone layout.
    {
        auto oit = project_plugin_origin_.find(plugin_name);
        apply_manifest_flags_(pi, oit != project_plugin_origin_.end()
                                      ? oit->second : pi.folder_path);
    }
    // Fail-check, BEFORE reloading: if the old module is still mapped, the
    // FreeLibrary in detach didn't drop its last ref (a lingering worker
    // thread / pinning dependency DLL), so the LoadLibrary below would just
    // bump the stale module's refcount and hand back the OLD code. Detect it
    // with a refcount-neutral module query. (Comparing the reloaded base to
    // the old base is unreliable — a fully-unloaded DLL usually remaps at its
    // same preferred base, which would false-positive every clean reload.)
    bool stale_module = false;
    if (old_base != nullptr) {
        HMODULE still = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               new_dll_path.c_str(), &still) && still != nullptr)
            stale_module = true;
    }
    HMODULE h = load_plugin_dll_(new_dll_path);
    if (!h) {
        if (err) *err = "LoadLibrary failed on rebuilt DLL — instances for "
                        "this plugin are gone; reopen the project to recover";
        return false;
    }
    pi.handle = h;

    std::string aerr;
    if (!plugin_abi_compatible(pi.handle, plugin_name, pi.json_fallback, &aerr)) {
        FreeLibrary(pi.handle);
        pi.handle = nullptr;
        if (err) *err = aerr + " — instances for this plugin are gone; "
                               "reopen the project to recover";
        return false;
    }
    // LV2-style capability handshake on the rebuilt DLL (the manifest was
    // re-read above, so a newly-declared required interface re-gates here too).
    if (!plugin_caps_compatible(pi, &default_host_api(), plugin_name, &aerr)) {
        FreeLibrary(pi.handle);
        pi.handle = nullptr;
        if (err) *err = aerr + " — instances for this plugin are gone; "
                               "reopen the project to recover";
        return false;
    }
    pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
        GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
    if (!pi.c_factory) {
        if (err) *err = "factory '" + pi.factory_symbol + "' not exported in "
                        "rebuilt DLL — instances for this plugin are gone; "
                        "reopen the project to recover";
        return false;
    }

    for (auto& p : pending) {
        auto inst = make_adapter_guarded_(pi, plugin_name, p.name, p.max_concurrency, p.on_fault);
        if (!inst) continue;
        if (!p.def_json.empty()) inst->set_def(p.def_json);
        project_.instances[p.name].instance = inst;
        InstanceRegistry::instance().add(inst);
    }
    // #18: stamp the change-gate ONLY on genuine success — AFTER the
    // stale_module check. If the old module is still pinned (NEW code is NOT
    // active), bumping loaded_dll_mtime here would poison the mtime gate so the
    // next Rebuild sees "unchanged" and never retries the reload → the plugin
    // runs stale code forever. The LoadLibrary/ABI/factory fail branches above
    // already return before any stamp; this matches them.
    if (stale_module) {
        if (err) *err = "DLL did not unload (same module base after reload) — "
                        "an instance likely left a worker thread or GPU context "
                        "alive; NEW code is NOT active";
        return false;
    }
    stamp_loaded_dll_(pi, new_dll_path);   // refresh change-gate (success only)
    return true;
}

inline int PluginManager::compile_project_plugins_locked(const std::string& project_folder) {
    std::vector<std::filesystem::directory_entry> folders;
    auto root = std::filesystem::path(project_folder) / "plugins";
    std::error_code ec;
    if (std::filesystem::exists(root))
        for (auto& e : std::filesystem::directory_iterator(root, ec))
            if (e.is_directory()) folders.push_back(e);
    return compile_plugin_folders_locked_(folders);
}

// Compile (cl.exe source / load cmake-prebuilt) + register each plugin folder
// as a TRUSTED project plugin (no cert; recompile/rebuild-able). Used for
// <project>/plugins/* and — when project.json `plugin_dirs_compile` is on —
// for plugin_dirs-resolved folders too. mu_ MUST be held.
inline int PluginManager::compile_plugin_folders_locked_(const std::vector<std::filesystem::directory_entry>& folders) {
    int ok_count = 0;
    for (auto& entry : folders) {
        std::string pname = entry.path().filename().string();
        try {
            // Build mode + DLL name from plugin.json, read up-front because
            // it decides whether we cl.exe-compile at all. A prebuilt/cmake
            // plugin owns its own build (its CMakeLists handles external libs
            // / CUDA); the backend never compiles it, it just loads the
            // plugin's named DLL.
            bool prebuilt = false;
            std::string want_dll = pname + ".dll";
            // #13: the plugins_ map (which holds the HMODULE) is keyed by the
            // MANIFEST name, but the drop-prior guard + close/open teardown used
            // to key by the FOLDER leaf — so a plugin whose plugin.json `name`
            // differs from its folder leaked its HMODULE and left a stale entry
            // shadowing the next project. Resolve the manifest name up-front and
            // use it as the ONE key everywhere below (plugins_, origin, teardown).
            std::string manifest_name = pname;
            {
                auto mpath = entry.path() / "plugin.json";
                if (std::filesystem::exists(mpath)) {
                    std::ifstream mf(mpath.string());
                    std::stringstream ms; ms << mf.rdbuf();
                    std::string mc = ms.str();
                    prebuilt = json_flag_true(mc, "prebuilt") ||
                               (extract_string(mc, "build").value_or("") == "cmake");
                    if (auto d = extract_string(mc, "dll")) want_dll = *d;
                    if (auto n = extract_string(mc, "name")) manifest_name = *n;
                }
            }

            // DM-9: a folder with a CMakeLists.txt but build != cmake is almost
            // certainly mis-declared — it cl.exe-compiles (and likely can't link
            // its external deps) instead of building via CMake, and 'Rebuild
            // Plugins' silently skips it (it only iterates prebuilt plugins).
            // Warn so the missing `"build": "cmake"` isn't a silent stale-code trap.
            if (!prebuilt && std::filesystem::exists(entry.path() / "CMakeLists.txt")) {
                last_open_warnings_.push_back({pname, pname,
                    "plugin has a CMakeLists.txt but no \"build\": \"cmake\" in plugin.json — "
                    "it won't be built by Rebuild Plugins; set build:cmake"});
                std::fprintf(stderr,
                    "[xinsp2] plugin '%s': CMakeLists.txt present but build != cmake — "
                    "set \"build\":\"cmake\"\n", pname.c_str());
            }

            // Collect .cpp sources: prefer src/ if present, else root.
            std::vector<std::string> sources;
            auto src_dir = entry.path() / "src";
            auto walk = [&](const std::filesystem::path& dir) { collect_cpp_sources(dir, sources); };
            if (std::filesystem::exists(src_dir)) walk(src_dir);
            else                                  walk(entry.path());
            if (!prebuilt && sources.empty()) {
                last_open_warnings_.push_back(
                    {pname, pname, "no .cpp sources found in project plugin folder"});
                std::fprintf(stderr,
                    "[xinsp2] project plugin '%s': no sources, skipped\n",
                    pname.c_str());
                continue;
            }

            // Optional include/ folder for plugin's own headers.
            std::vector<std::string> includes;
            auto inc_dir = entry.path() / "include";
            if (std::filesystem::exists(inc_dir)) {
                includes.push_back(inc_dir.string());
            }

            xi::script::CompileRequest req;
            if (!sources.empty()) {
                req.source_path = sources.front();
                req.extra_sources.assign(sources.begin() + 1, sources.end());
            }
            req.include_dirs   = includes;
            req.output_dir     = (entry.path() / "build").string();
            req.include_dir    = compile_env_.include_dir;
            req.vcvars_path    = compile_env_.vcvars_path;
            req.opencv_dir     = compile_env_.opencv_dir;
            req.turbojpeg_root = compile_env_.turbojpeg_root;
            req.ipp_root       = compile_env_.ipp_root;
            req.mode           = xi::script::CompileMode::PluginDev;

            xi::script::CompileResult res;
            if (compile_env_.aot || prebuilt) {
                // AOT bundle OR a cmake/prebuilt plugin: load a prebuilt DLL,
                // no cl.exe. A cmake plugin emits its DLL either into build/
                // (the in-tree convention) or next to plugin.json (the
                // standalone xinsp2_add_plugin default), so load the
                // *named* DLL from either — never "newest .dll", which could
                // pick up a shipped dependency DLL. AOT keeps its legacy
                // newest-in-build/ behaviour for export bundles.
                std::filesystem::path found;
                if (prebuilt) {
                    for (auto cand : { entry.path() / "build" / want_dll,
                                       entry.path() / want_dll }) {
                        if (std::filesystem::exists(cand)) { found = cand; break; }
                    }
                } else {
                    std::filesystem::path build_dir = entry.path() / "build";
                    std::filesystem::file_time_type best{};
                    if (std::filesystem::exists(build_dir))
                        for (auto& f : std::filesystem::directory_iterator(build_dir))
                            if (f.is_regular_file() && f.path().extension() == ".dll") {
                                auto t = std::filesystem::last_write_time(f);
                                if (found.empty() || t > best) { best = t; found = f.path(); }
                            }
                }
                if (found.empty()) {
                    if (prebuilt) {
                        // Register an unbuilt cmake plugin (no handle) so it's
                        // listed AND `rebuild_plugins` can build + load it later.
                        auto mpath = entry.path() / "plugin.json";
                        PluginInfo stub = std::filesystem::exists(mpath)
                            ? parse_manifest(mpath.string(), entry.path().string())
                            : PluginInfo{};
                        if (stub.name.empty()) stub.name = manifest_name;
                        if (stub.dll_name.empty()) stub.dll_name = want_dll;
                        if (stub.factory_symbol.empty()) stub.factory_symbol = "xi_plugin_create";
                        stub.prebuilt = true;
                        stub.description = "Project plugin (cmake, not built): " + pname;
                        // Key by the manifest name (the plugins_ key everywhere)
                        // and track it for teardown (#13).
                        std::string key = stub.name;
                        plugins_[key] = std::move(stub);
                        project_plugin_origin_[key] = entry.path().string();
                        project_loaded_plugins_.insert(key);
                        last_open_warnings_.push_back(
                            {pname, pname, "build:cmake plugin not built yet — run 'Rebuild Plugins'"});
                        std::fprintf(stderr,
                            "[xinsp2] cmake plugin '%s' registered (not built) — run Rebuild Plugins\n",
                            pname.c_str());
                    } else {
                        last_open_warnings_.push_back({pname, pname, "AOT: no prebuilt DLL in build/ (export first)"});
                        std::fprintf(stderr, "[xinsp2] AOT: plugin '%s' has no prebuilt DLL — skipped\n", pname.c_str());
                    }
                    continue;
                }
                res.ok = true; res.dll_path = found.string();
                std::fprintf(stderr, "[xinsp2] loading prebuilt plugin '%s': %s\n", pname.c_str(), res.dll_path.c_str());
            } else {
                std::fprintf(stderr,
                    "[xinsp2] compiling project plugin '%s' (%zu source%s)...\n",
                    pname.c_str(), sources.size(), sources.size() == 1 ? "" : "s");
                res = xi::script::compile(req);
                if (!res.ok) {
                    last_open_warnings_.push_back(
                        {pname, pname, "compile failed (see Output for details)"});
                    std::fprintf(stderr,
                        "[xinsp2] project plugin '%s' compile FAILED:\n%s\n",
                        pname.c_str(), res.build_log.c_str());
                    continue;
                }
            }

            // Drop any prior version of this same project plugin
            // (e.g., older DLL still loaded from a previous open). #13: look it
            // up by the MANIFEST name — the key that actually holds the handle —
            // not the folder leaf, or a renamed plugin's old HMODULE survives.
            auto prev = plugins_.find(manifest_name);
            if (prev != plugins_.end() && prev->second.handle) {
                FreeLibrary(prev->second.handle);   // TODO(linux): dlclose
                prev->second.handle = nullptr;
                prev->second.c_factory = nullptr;
            }

            PluginInfo pi;
            pi.name           = manifest_name;
            pi.description    = "Project plugin: " + pname;
            pi.dll_name       = std::filesystem::path(res.dll_path).filename().string();
            pi.factory_symbol = "xi_plugin_create";
            pi.folder_path    = std::filesystem::path(res.dll_path).parent_path().string();
            pi.prebuilt       = prebuilt;
            stamp_loaded_dll_(pi, res.dll_path);
            // Optional plugin.json overrides
            auto manifest = entry.path() / "plugin.json";
            if (std::filesystem::exists(manifest)) {
                std::ifstream mf(manifest.string());
                std::stringstream ms; ms << mf.rdbuf();
                std::string mc = ms.str();
                if (auto n = extract_string(mc, "name"))        pi.name = *n;
                if (auto d = extract_string(mc, "description")) pi.description = *d;
                pi.has_ui = (mc.find("\"has_ui\":true") != std::string::npos) ||
                            (mc.find("\"has_ui\": true") != std::string::npos);
                // reentrant / json_fallback / is_sink / factory via the SHARED
                // helper so this path and the reload paths agree (#20/#23).
                apply_manifest_flags_from_content_(pi, mc);
                if (pi.has_ui) pi.ui_path = (entry.path() / "ui").string();
                std::string mblock;
                if (detail_find_key(mc, "manifest", mblock)) pi.manifest_json = std::move(mblock);
            }
            // Load the freshly built DLL up-front. Project plugins
            // skip the cert/baseline gate — they are inside the
            // user's own project, not third-party code, so we trust
            // the source. (Export will run cert.)
            auto dll_path = std::filesystem::path(pi.folder_path) / pi.dll_name;
            // Search the plugin's own folder for its dependency DLLs (see
            // load_plugin_dll_ for the search-flag rationale).
            pi.handle = load_plugin_dll_(dll_path.string());
            if (!pi.handle) {
                last_open_warnings_.push_back(
                    {pname, pname, "DLL built but LoadLibrary failed"});
                std::fprintf(stderr,
                    "[xinsp2] project plugin '%s': LoadLibrary failed\n",
                    pname.c_str());
                continue;
            }
            {
                std::string err;
                if (!plugin_abi_compatible(pi.handle, pname, pi.json_fallback, &err)) {
                    last_open_warnings_.push_back({pname, pname, err});
                    std::fprintf(stderr, "[xinsp2] %s\n", err.c_str());
                    FreeLibrary(pi.handle);
                    pi.handle = nullptr;
                    continue;
                }
                // LV2-style capability handshake — same refuse-with-reason path.
                if (!plugin_caps_compatible(pi, &default_host_api(), pname, &err)) {
                    last_open_warnings_.push_back({pname, pname, err});
                    std::fprintf(stderr, "[xinsp2] %s\n", err.c_str());
                    FreeLibrary(pi.handle);
                    pi.handle = nullptr;
                    continue;
                }
            }
            pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
            if (!pi.c_factory) {
                last_open_warnings_.push_back(
                    {pname, pname,
                     "DLL loaded but factory '" + pi.factory_symbol + "' not found"});
                std::fprintf(stderr,
                    "[xinsp2] project plugin '%s': factory '%s' not exported\n",
                    pname.c_str(), pi.factory_symbol.c_str());
                FreeLibrary(pi.handle);
                pi.handle = nullptr;
                continue;
            }
            // #13: key origin + teardown by the SAME manifest name that holds
            // the handle in plugins_ — not the folder leaf.
            std::string key = pi.name;
            plugins_[key] = std::move(pi);
            project_plugin_origin_[key] = entry.path().string();
            project_loaded_plugins_.insert(key);
            ok_count++;
        } catch (const std::exception& e) {
            last_open_warnings_.push_back(
                {pname, pname, std::string("exception: ") + e.what()});
            std::fprintf(stderr,
                "[xinsp2] project plugin '%s' threw: %s\n",
                pname.c_str(), e.what());
        }
    }
    return ok_count;
}

// Hot-rebuild one project plugin and re-instantiate every instance
// using it, preserving each instance's saved def. Used by the
// extension's file watcher: edit plugin .cpp → save → backend
// recompiles + reloads → next trigger uses the new code, no
// backend restart needed.
//
// Returns: ok flag, build log, list of instance names that were
// re-instantiated. On compile failure the OLD DLL stays loaded so
// running inspection isn't disrupted.
inline PluginManager::RecompileResult PluginManager::recompile_project_plugin(const std::string& plugin_name) {
    std::lock_guard<std::mutex> lk(mu_);
    RecompileResult r;
    auto orig_it = project_plugin_origin_.find(plugin_name);
    if (orig_it == project_plugin_origin_.end()) {
        r.error = "not a project plugin: " + plugin_name;
        return r;
    }
    std::string source_dir = orig_it->second;

    // 1. Cache each instance's def, then destroy. We keep them in a
    //    pending list and rebuild after the new DLL is loaded.
    struct Pending {
        std::string name;
        std::string folder;
        std::string def_json;
        int         max_concurrency = 0;
        OnFault     on_fault = OnFault::Reuse;   // item 14: preserved across recompile
    };
    std::vector<Pending> pending;
    for (auto& [iname, ii] : project_.instances) {
        if (ii.plugin_name != plugin_name) continue;
        Pending p;
        p.name   = iname;
        p.folder = ii.folder_path;
        p.max_concurrency = ii.max_concurrency;
        p.on_fault = ii.on_fault;
        if (ii.instance) p.def_json = ii.instance->get_def();
        pending.push_back(std::move(p));
    }
    for (auto& p : pending) {
        auto& ii = project_.instances[p.name];
        InstanceRegistry::instance().remove(p.name);
        ii.instance.reset();   // dtor calls xi_plugin_destroy
    }

    // B-P1-4: any error-return after step 1 must put the instances
    // back into the dict using the OLD (still-loaded) DLL — the
    // alternative is leaving project_.instances[name].instance
    // null and subsequent calls silently no-op. The compile-
    // failure branch already does this; lift it into a lambda so
    // every other early-return path does too.
    auto restore_against_old = [&]() {
        auto pi_it = plugins_.find(plugin_name);
        if (pi_it == plugins_.end()) return;
        auto& pi_old = pi_it->second;
        if (!pi_old.c_factory) return;
        for (auto& p : pending) {
            auto inst = make_adapter_guarded_(pi_old, plugin_name, p.name, p.max_concurrency, p.on_fault);
            if (!inst) continue;
            if (!p.def_json.empty()) inst->set_def(p.def_json);
            project_.instances[p.name].instance = inst;
            InstanceRegistry::instance().add(inst);
            r.reattached_instances.push_back(p.name);
        }
    };

    // 2. Compile fresh into the same plugin folder. We don't drop
    //    the old DLL until the new one is ready, so a compile
    //    failure leaves the project in its previous working state.
    auto plugin_dir = std::filesystem::path(source_dir);
    std::vector<std::string> sources;
    auto walk = [&](const std::filesystem::path& dir) { collect_cpp_sources(dir, sources); };
    auto src_subdir = plugin_dir / "src";
    if (std::filesystem::exists(src_subdir)) walk(src_subdir);
    else                                     walk(plugin_dir);
    if (sources.empty()) {
        r.error = "no .cpp sources in " + plugin_dir.string();
        restore_against_old();   // B-P1-4
        return r;
    }
    std::vector<std::string> includes;
    auto inc_dir = plugin_dir / "include";
    if (std::filesystem::exists(inc_dir)) includes.push_back(inc_dir.string());

    xi::script::CompileRequest req;
    req.source_path    = sources.front();
    req.extra_sources.assign(sources.begin() + 1, sources.end());
    req.include_dirs   = includes;
    req.output_dir     = (plugin_dir / "build").string();
    req.include_dir    = compile_env_.include_dir;
    req.vcvars_path    = compile_env_.vcvars_path;
    req.opencv_dir     = compile_env_.opencv_dir;
    req.turbojpeg_root = compile_env_.turbojpeg_root;
    req.ipp_root       = compile_env_.ipp_root;
    req.mode           = xi::script::CompileMode::PluginDev;

    auto cres = xi::script::compile(req);
    r.build_log   = cres.build_log;
    r.diagnostics = cres.diagnostics;
    if (!cres.ok) {
        r.error = "compile failed";
        // Re-instantiate against the OLD DLL so we don't leave the
        // project broken. Old DLL is still loaded since we never
        // FreeLibrary'd it.
        auto pi_it = plugins_.find(plugin_name);
        if (pi_it != plugins_.end() && pi_it->second.c_factory) {
            for (auto& p : pending) {
                auto inst = make_adapter_guarded_(pi_it->second, plugin_name, p.name, p.max_concurrency, p.on_fault);
                if (!inst) continue;
                if (!p.def_json.empty()) inst->set_def(p.def_json);
                project_.instances[p.name].instance = inst;
                InstanceRegistry::instance().add(inst);
                r.reattached_instances.push_back(p.name);
            }
        }
        return r;
    }

    // 3. Compile succeeded — swap DLLs.
    auto pi_it = plugins_.find(plugin_name);
    if (pi_it == plugins_.end()) {
        r.error = "internal: plugin entry vanished mid-recompile";
        // B-P1-4: nothing to restore against (old plugin entry
        // gone), but the bookkeeping for `r` is still correct.
        return r;
    }
    auto& pi = pi_it->second;
    if (pi.handle) {
        FreeLibrary(pi.handle);
        pi.handle    = nullptr;
        pi.c_factory = nullptr;
    }
    // After the FreeLibrary above, restore_against_old() can no
    // longer save us — the old DLL is gone. The remaining error
    // returns below leave instances null deliberately; that's
    // strictly worse than not freeing the old DLL, but keeping
    // the old DLL in memory while the user just asked for a
    // recompile is also wrong (subsequent calls would land in the
    // old code, contradicting the user's intent). Document the
    // tradeoff via a clear "rebuild the project" error message
    // rather than silently no-op'ing.
    pi.dll_name    = std::filesystem::path(cres.dll_path).filename().string();
    pi.folder_path = std::filesystem::path(cres.dll_path).parent_path().string();
    // #20: re-read the dispatch-flag manifest from the SOURCE folder before
    // re-resolving the factory + rebuilding adapters, so a hot recompile after
    // toggling reentrant/is_sink/json_fallback/factory in plugin.json applies the
    // NEW semantics (a now-non-reentrant plugin starts serializing). The prior
    // code reused the stale `pi` flags. Shared helper → agrees with full build +
    // cmake reattach.
    apply_manifest_flags_(pi, source_dir);
    // Search the plugin's own folder for its dependency DLLs (see
    // load_plugin_dll_ for the rationale).
    pi.handle = load_plugin_dll_(cres.dll_path);
    if (!pi.handle) {
        r.error = "LoadLibrary failed on freshly-built DLL — instances "
                  "for this plugin are gone; reopen the project to "
                  "recover";
        return r;
    }
    {
        std::string err;
        if (!plugin_abi_compatible(pi.handle, plugin_name, pi.json_fallback, &err)) {
            r.error = err + " — instances for this plugin are gone; "
                            "reopen the project to recover";
            FreeLibrary(pi.handle);
            pi.handle = nullptr;
            return r;
        }
        // LV2-style capability handshake on the freshly-built DLL (manifest
        // re-read just above) — refuse with reason on an unsatisfied require.
        if (!plugin_caps_compatible(pi, &default_host_api(), plugin_name, &err)) {
            r.error = err + " — instances for this plugin are gone; "
                            "reopen the project to recover";
            FreeLibrary(pi.handle);
            pi.handle = nullptr;
            return r;
        }
    }
    pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
        GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
    if (!pi.c_factory) {
        r.error = "factory '" + pi.factory_symbol + "' not exported in new DLL"
                  " — instances for this plugin are gone; reopen the "
                  "project to recover";
        return r;
    }

    // Refresh the change-gate stamp so a later reload_changed_plugins()
    // doesn't see this freshly-recompiled DLL as "changed" and swap it again.
    stamp_loaded_dll_(pi, cres.dll_path);

    // 4. Re-instantiate every preserved instance using the new factory.
    for (auto& p : pending) {
        auto inst = make_adapter_guarded_(pi, plugin_name, p.name, p.max_concurrency, p.on_fault);
        if (!inst) continue;
        if (!p.def_json.empty()) inst->set_def(p.def_json);
        project_.instances[p.name].instance = inst;
        InstanceRegistry::instance().add(inst);
        r.reattached_instances.push_back(p.name);
    }
    r.ok = true;
    return r;
}

// Backend-driven "Rebuild Plugins": for every cmake/prebuilt plugin whose
// source changed, unload it (releasing the loaded DLL's file lock), run its
// own CMake build, then load the rebuilt DLL and restore each instance's def.
// Unchanged plugins (all sources older than their built DLL) are skipped, so
// a CUDA/heavy-state plugin you didn't touch keeps its context.
//
// The unload→build→load ordering is REQUIRED on Windows: a loaded DLL can't
// be overwritten, and CMake emits a fixed-name DLL (unlike the cl.exe path,
// which versions the filename to dodge the lock). That's also why CMake runs
// here in the backend rather than client-side — only the host can release the
// lock around the build while holding each instance's def for replay.
//
// Three phases so a multi-plugin integration round (the common case) is fast
// and each plugin is unloaded only as briefly as possible:
//   A. unload ALL changed plugins (release every DLL lock) up front,
//   B. build them in PARALLEL (independent cmake invocations),
//   C. reload each + restore its instances.
//
// `only` (optional) restricts the set to those plugin names — the extension
// passes it to rebuild just the one(s) you're iterating on. Empty = all
// cmake plugins.
//
// Caller MUST quiesce dispatch first. cmake_exe = "cmake" (or an absolute
// path); config = "Release".
inline PluginManager::PluginRebuildReport PluginManager::rebuild_cmake_plugins(const std::string& cmake_exe,
                                          const std::string& config,
                                          const std::vector<std::string>& only) {
    std::lock_guard<std::mutex> lk(mu_);
    PluginRebuildReport rep;
    // XINSP2_ROOT for a plugin's CMakeLists = <root> from <root>/backend/include.
    std::string xinsp_root =
        std::filesystem::path(compile_env_.include_dir).parent_path().parent_path().string();
    std::unordered_set<std::string> filter(only.begin(), only.end());

    struct Job {
        std::string name, src_dir;
        bool loaded = false;
        HMODULE old_base = nullptr;
        std::vector<PendingInstance> pending;
        std::string log;
        int rc = 0;
    };
    std::vector<Job> jobs;

    // Decide the changed set (mtime gate) + record unchanged/skip reasons.
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (auto& [n, pi] : plugins_) if (pi.prebuilt) names.push_back(n);
    for (auto& name : names) {
        if (!filter.empty() && !filter.count(name)) continue;
        auto it = plugins_.find(name);
        if (it == plugins_.end()) continue;
        auto oit = project_plugin_origin_.find(name);
        std::string src_dir = (oit != project_plugin_origin_.end())
                                  ? oit->second : it->second.folder_path;
        if (!std::filesystem::exists(std::filesystem::path(src_dir) / "CMakeLists.txt")) {
            rep.items.push_back({name, "failed",
                "build:cmake plugin has no CMakeLists.txt in " + src_dir});
            continue;
        }
        bool loaded = (it->second.handle != nullptr);
        if (loaded && xi::cmake_build::newest_source_mtime(src_dir) <= it->second.loaded_dll_mtime) {
            rep.items.push_back({name, "unchanged", ""});
            continue;
        }
        Job j; j.name = name; j.src_dir = src_dir; j.loaded = loaded;
        jobs.push_back(std::move(j));
    }

    // Phase A — unload every changed plugin (frees all the DLL file locks).
    for (auto& j : jobs)
        if (j.loaded) j.pending = detach_plugin_instances_locked_(j.name, &j.old_base);

    // Phase B — build them in parallel. build_cmake_plugin_ only reads
    // compile_env_ + touches the filesystem + spawns cmake, so concurrent
    // calls over distinct dirs don't race (plugins_ isn't touched here).
    {
        std::vector<std::future<void>> futs;
        futs.reserve(jobs.size());
        for (size_t i = 0; i < jobs.size(); ++i) {
            futs.push_back(std::async(std::launch::async,
                [this, i, &jobs, &cmake_exe, &config, &xinsp_root]() {
                    auto& j = jobs[i];
                    j.rc = xi::cmake_build::build_cmake_plugin(
                        cmake_exe, j.src_dir, config, xinsp_root,
                        compile_env_.opencv_dir, j.log);
                    // cmake/MSBuild/cl write OEM-codepage bytes (CP950 on zh-TW)
                    // — scrub to valid UTF-8 so a failure log never breaks the
                    // WS text frame.
                    j.log = xi::script::ensure_utf8(j.log);
                }));
        }
        for (auto& f : futs) f.get();
    }

    // Phase C — reload each + restore instances (sequential; mutates plugins_).
    for (auto& j : jobs) {
        auto it = plugins_.find(j.name);
        if (it == plugins_.end()) { rep.items.push_back({j.name, "failed", "plugin entry vanished"}); continue; }
        if (j.rc != 0) {
            // Build failed — reattach against the still-on-disk old DLL so the
            // plugin keeps running on old code; report the failure.
            auto dll_path = (std::filesystem::path(it->second.folder_path) /
                             it->second.dll_name).string();
            std::string e2;
            if (j.loaded && std::filesystem::exists(dll_path))
                reattach_plugin_from_dll_locked_(j.name, dll_path, j.pending, j.old_base, &e2);
            rep.items.push_back({j.name, "failed", "cmake build failed:\n" + j.log});
            continue;
        }
        // Locate the rebuilt DLL: xinsp2_add_plugin emits next to CMakeLists
        // (RUNTIME_OUTPUT_DIRECTORY = source dir); also accept build/ layouts.
        std::string want = it->second.dll_name;
        std::filesystem::path built;
        for (auto cand : { std::filesystem::path(j.src_dir) / want,
                           std::filesystem::path(j.src_dir) / "build" / want,
                           std::filesystem::path(j.src_dir) / "build" / config / want }) {
            if (std::filesystem::exists(cand)) { built = cand; break; }
        }
        if (built.empty()) {
            rep.items.push_back({j.name, "failed",
                "build OK but no " + want + " found under " + j.src_dir});
            continue;
        }
        it->second.folder_path = built.parent_path().string();
        it->second.dll_name    = built.filename().string();
        std::string err;
        bool ok = reattach_plugin_from_dll_locked_(j.name, built.string(),
                                                   j.pending, j.old_base, &err);
        rep.items.push_back({j.name, ok ? "rebuilt" : "failed", ok ? built.string() : err});
    }
    return rep;
}

// Export a project plugin as a standalone deployable folder. The packaging
// logic lives in xi_plugin_export.hpp (export_project_plugin_impl) — a
// self-contained build concern. This wrapper just takes the lock, resolves
// the plugin's source dir + manifest info, and delegates.
inline PluginManager::ExportResult PluginManager::export_project_plugin(const std::string& plugin_name,
                                    const std::string& dest_root) {
    std::lock_guard<std::mutex> lk(mu_);
    ExportResult er;
    auto orig_it = project_plugin_origin_.find(plugin_name);
    if (orig_it == project_plugin_origin_.end()) {
        er.error = "not a project plugin: " + plugin_name;
        return er;
    }
    auto pi_it = plugins_.find(plugin_name);
    if (pi_it == plugins_.end()) {
        er.error = "plugin entry missing: " + plugin_name;
        return er;
    }
    return xi::export_project_plugin_impl(
        plugin_name, std::filesystem::path(orig_it->second),
        pi_it->second, compile_env_, dest_root);
}

// Was this plugin loaded from inside the current project (vs. global)?
inline bool PluginManager::is_project_plugin(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    return project_plugin_origin_.count(name) > 0;
}

// Load a plugin's DLL and resolve the factory function.
//
// Plugins are trusted: the DLL is loaded straight through after an ABI
// compatibility check (no baseline cert gate — removed 2026-06).
// err (optional): on failure, filled with a human-readable reason so callers
// (the create_instance handler) can surface WHY instead of a generic message.
inline bool PluginManager::load_plugin(const std::string& name, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    return load_plugin_locked_(name, err);
}

// Body of load_plugin with mu_ ALREADY held — so autoload (which holds mu_ for
// the whole reconcile) can bring a scanned-but-unloaded lib DLL into memory
// without re-entering the non-recursive mu_.
inline bool PluginManager::load_plugin_locked_(const std::string& name, std::string* err) {
    auto fail = [&](std::string msg) { if (err) *err = std::move(msg); return false; };
    auto it = plugins_.find(name);
    if (it == plugins_.end())
        return fail("plugin '" + name + "' not found (not in any plugins dir or the open project)");
    auto& pi = it->second;
    if (pi.handle) return true; // already loaded

    auto dll_path = std::filesystem::path(pi.folder_path) / pi.dll_name;
    if (!std::filesystem::exists(dll_path))
        return fail("plugin '" + name + "': built DLL not found at " + dll_path.string() +
                    " - it has source but no compiled DLL. Open it as a project plugin "
                    "(copy into <project>/plugins/ + open_project) to compile from source, "
                    "or build + certify it for the scan path.");

    pi.handle = load_plugin_dll_(dll_path.string());
    if (!pi.handle)
        return fail("plugin '" + name + "': LoadLibrary failed for " + dll_path.string() +
                    " (Windows error " + std::to_string(GetLastError()) + ")");

    {
        std::string aerr;
        if (!plugin_abi_compatible(pi.handle, name, pi.json_fallback, &aerr)) {
            std::fprintf(stderr, "[xinsp2] %s\n", aerr.c_str());
            FreeLibrary(pi.handle);
            pi.handle = nullptr;
            return fail(aerr);
        }
        // LV2-style required/optional capability handshake (core_fix_plan.md
        // §12 Phase 3). Validate the manifest's `requires[]` against the same
        // host table the factory will receive (default_host_api's get_interface
        // door); a missing/too-old required interface refuses via the same
        // refuse-with-reason channel as the ABI gate above. Optional ifaces are
        // not gated — the plugin null-checks them at runtime.
        if (!plugin_caps_compatible(pi, &default_host_api(), name, &aerr)) {
            std::fprintf(stderr, "[xinsp2] %s\n", aerr.c_str());
            FreeLibrary(pi.handle);
            pi.handle = nullptr;
            return fail(aerr);
        }
    }

    // C ABI factory takes (xi_host_api*, const char*) → void*.
    // Plugins are trusted (no baseline cert gate — removed 2026-06; in-
    // process plugins load straight through, speed-first).
    pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
        GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
    if (pi.c_factory == nullptr)
        return fail("plugin '" + name + "': factory symbol '" + pi.factory_symbol +
                    "' not found in the DLL");
    stamp_loaded_dll_(pi, dll_path.string());   // change-gate for reload_changed
    return true;
}

// List all discovered plugins (loaded or not).
inline std::vector<PluginInfo> PluginManager::list_plugins() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PluginInfo> out;
    for (auto& [k, v] : plugins_) out.push_back(v);
    return out;
}

inline PluginInfo* PluginManager::find_plugin(const std::string& name) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = plugins_.find(name);
    return it == plugins_.end() ? nullptr : &it->second;
}

// Part III G2.1 — resolve a plugin's on-disk location (folder + dll filename)
// by name, copying under the lock so the caller never derefs a map pointer that
// a concurrent rebuild could invalidate. Used to stamp the crash culprit (so the
// FE can quarantine the offending DLL via G1's .xi_certify.json). Returns false
// if the plugin isn't registered.
inline bool PluginManager::plugin_location(const std::string& name, std::string& folder, std::string& dll) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = plugins_.find(name);
    if (it == plugins_.end()) return false;
    folder = it->second.folder_path;
    dll    = it->second.dll_name;
    return true;
}

} // namespace xi
