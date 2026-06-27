#pragma once
//
// xi_plugin_manager.hpp — discovers, loads, and manages plugins.
//
// A plugin is a folder under the plugins/ directory containing:
//   plugin.json  — manifest (name, description, factory symbol)
//   <name>.dll   — shared library exporting a factory function
//   ui/          — optional web UI bundle (index.html + assets)
//
// Plugin manifest format (plugin.json):
// {
//   "name":        "mock_camera",
//   "description": "Simulated camera for testing",
//   "dll":         "mock_camera.dll",
//   "factory":     "xi_plugin_create",    // exported C function
//   "has_ui":      true
// }
//
// Factory signature:
//   extern "C" __declspec(dllexport)
//   xi::InstanceBase* xi_plugin_create(const char* instance_name);
//
// The returned object is owned by the caller (the PluginManager wraps
// it in a shared_ptr).
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
#include "xi_cabi_adapter.hpp" // plugin_abi_compatible / PluginInfo / CAbiInstanceAdapter
#include "xi_image_pool.hpp"
#include "xi_instance.hpp"
#include "xi_config_validate.hpp" // validate_config_against_manifest (opt-in diagnostic, extracted leaf)
#include "xi_pm_json.hpp"      // pm_json_escape / pm_json_quote (extracted leaf)
#include <cctype>
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
        // Release every loaded plugin DLL on process shutdown. In practice
        // the OS would reclaim these, but freeing explicitly keeps leak
        // detectors clean and avoids surprises if a plugin registers
        // static destructors.
        for (auto& [name, pi] : plugins_) {
            if (pi.handle) {
                FreeLibrary(pi.handle);
                pi.handle = nullptr;
            }
        }
    }

    // Scan a directory for plugin folders. Each subfolder with a plugin.json
    // is registered. An already-loaded plugin (handle != nullptr) keeps its
    // handle and resolved factory — we refresh only manifest metadata so
    // rescan_plugins doesn't leak the prior HMODULE.
    int scan_plugins(const std::string& plugins_dir) {
        std::lock_guard<std::mutex> lk(mu_);
        int count = 0;
        if (!std::filesystem::exists(plugins_dir)) return 0;
        for (auto& entry : std::filesystem::directory_iterator(plugins_dir)) {
            if (!entry.is_directory()) continue;
            if (register_plugin_folder_locked_(entry.path().string())) count++;
        }
        return count;
    }

    // Register a single plugin folder (one that contains plugin.json) into the
    // registry. An already-loaded plugin of the same name keeps its live handle +
    // factories — we refresh only the metadata that can change between scans, so
    // re-registering doesn't leak the prior HMODULE. mu_ MUST be held.
    bool register_plugin_folder_locked_(const std::string& folder) {
        auto manifest = std::filesystem::path(folder) / "plugin.json";
        if (!std::filesystem::exists(manifest)) return false;
        auto info = parse_manifest(manifest.string(), folder);
        if (info.name.empty()) return false;
        auto existing = plugins_.find(info.name);
        if (existing != plugins_.end() && existing->second.handle) {
            existing->second.description   = info.description;
            existing->second.has_ui        = info.has_ui;
            existing->second.reentrant     = info.reentrant;
            existing->second.prebuilt      = info.prebuilt;
            existing->second.ui_path       = info.ui_path;
            existing->second.folder_path   = info.folder_path;
            existing->second.manifest_json = info.manifest_json;
        } else {
            plugins_[info.name] = std::move(info);
        }
        return true;
    }

    // Expand a project.json plugin_dir entry to an absolute search root. Portable
    // forms only: `${ENV}` substitution, leading `~`, and relative paths (resolved
    // against the project folder). Absolute paths pass through. This is what keeps
    // the committed project.json machine-independent — absolute machine roots live
    // in env vars, not in the file.
    static std::string expand_plugin_root_(std::string r, const std::string& project_folder) {
        for (size_t p; (p = r.find("${")) != std::string::npos; ) {
            auto e = r.find('}', p);
            if (e == std::string::npos) break;
            std::string var = r.substr(p + 2, e - p - 2);
            const char* val = std::getenv(var.c_str());
            r.replace(p, e - p + 1, val ? val : "");
        }
        if (!r.empty() && r[0] == '~') {
            const char* home = std::getenv("USERPROFILE");      // TODO(linux): $HOME
            if (!home) home = std::getenv("HOME");
            if (home) r = std::string(home) + r.substr(1);
        }
        std::filesystem::path p(r);
        if (p.is_relative()) p = std::filesystem::path(project_folder) / p;
        return p.lexically_normal().string();
    }

    // Resolve project.json's external plugin coordinates: for each `plugins`
    // entry's `path`, search each (expanded) `plugin_dirs` root in order and take
    // the first `<root>/<path>/plugin.json` found (makefile-/$PATH-style
    // first-match-wins). Per ref, `compile` decides the treatment:
    //   false = only REGISTER (must be prebuilt or build:cmake);
    //   true  = treat the folder exactly like a `<project>/plugins/` one —
    //           cl.exe-compile source / load cmake-prebuilt, TRUSTED (no cert),
    //           recompile/rebuild-able. The "plugin toolbox" dev case.
    // Unresolved refs become open warnings listing the roots searched. Runs AFTER
    // project-local plugins are built so a same-named project plugin wins. Relative
    // roots resolve against the project folder (so a local `plugins` root needs no
    // `./`). mu_ MUST be held.
    void resolve_external_project_plugins_locked_(
            const std::string& project_folder,
            const std::vector<std::string>& dirs_raw,
            const std::vector<ProjectInfo::PluginRef>& refs) {
        if (refs.empty()) return;
        std::vector<std::string> roots;
        roots.reserve(dirs_raw.size());
        for (auto& d : dirs_raw) roots.push_back(expand_plugin_root_(d, project_folder));
        std::vector<std::filesystem::directory_entry> to_compile;
        for (auto& ref : refs) {
            std::filesystem::path found;
            for (auto& root : roots) {
                auto cand = std::filesystem::path(root) / ref.path;
                if (std::filesystem::exists(cand / "plugin.json")) { found = cand; break; }
            }
            if (found.empty()) {
                std::string searched;
                for (auto& root : roots)
                    searched += "\n  - " + (std::filesystem::path(root) / ref.path).string();
                last_open_warnings_.push_back({ref.label, "",
                    "plugin '" + ref.path + "' not found in any plugin_dir; searched:" + searched +
                    (roots.empty() ? "\n  (no plugin_dirs declared)" : "")});
                std::fprintf(stderr, "[xinsp2] plugin '%s' (%s) not resolved against %zu plugin_dir(s)\n",
                             ref.label.c_str(), ref.path.c_str(), roots.size());
                continue;
            }
            std::fprintf(stderr, "[xinsp2] resolved plugin '%s' -> %s%s\n",
                         ref.label.c_str(), found.string().c_str(), ref.compile ? " (compile)" : "");
            if (ref.compile) {
                to_compile.emplace_back(found);   // build + register as a trusted project plugin
            } else if (!register_plugin_folder_locked_(found.string())) {
                last_open_warnings_.push_back({ref.label, "",
                    "resolved folder has no valid plugin.json: " + found.string()});
            }
        }
        if (!to_compile.empty()) compile_plugin_folders_locked_(to_compile);
    }

    // Compile and register every plugin under <project>/plugins/. Each
    // subfolder is one project-local plugin; we accept whichever shape the
    // author prefers:
    //
    //   plugins/my_plugin/plugin.cpp                       (single file)
    //   plugins/my_plugin/{plugin.cpp, helpers.cpp}        (multi file at root)
    //   plugins/my_plugin/src/*.cpp                         (src/ subdir)
    //   plugins/my_plugin/plugin.json                       (optional manifest)
    //
    // The DLL is built into <plugin_folder>/build/<name>.dll with PluginDev
    // codegen flags (debugger-friendly /Od /Zi /RTC1) so the developer can
    // F5-attach the backend and step through plugin source as if it were
    // part of the main project.
    //
    // Each per-plugin compile is independent — a build failure on one
    // plugin records a warning and continues with the rest. Surfaced via
    // open_warnings() the same way bad instances are.
    //
    // Returns the count of successfully compiled+loaded project plugins.
    int compile_project_plugins(const std::string& project_folder) {
        std::lock_guard<std::mutex> lk(mu_);
        return compile_project_plugins_locked(project_folder);
    }

private:
    // Record the on-disk write-time + size of the DLL we just loaded, so
    // reload_changed_plugins() can later tell whether a rebuild produced a new
    // DLL (and only hot-swap the ones that actually moved).
    static void stamp_loaded_dll_(PluginInfo& pi, const std::string& dll_path) {
        std::error_code ec;
        auto sz = std::filesystem::file_size(dll_path, ec);
        pi.loaded_dll_size = ec ? 0 : (uint64_t)sz;
        auto wt = std::filesystem::last_write_time(dll_path, ec);
        pi.loaded_dll_mtime = ec ? 0 : (uint64_t)wt.time_since_epoch().count();
    }

    // Host-side cmake invocation (newest_source_mtime / run_cmd_capture /
    // build_cmake_plugin) moved to xi_cmake_build.hpp (xi::cmake_build::;
    // included above). rebuild_cmake_plugins below orchestrates them.

    // One instance preserved across a plugin reload: its name + folder +
    // serialized def, captured before destruction and replayed after reload.
    struct PendingInstance {
        std::string name, folder, def_json;
        int         max_concurrency = 0;
    };

    // Owner-guarded factory call shared by every (re)instantiation path on the
    // reload/recompile lanes. Pre-allocates an ImagePool owner id and wraps the
    // ctor in an OwnerGuard so images the ctor allocates are tagged, then either
    // adopted by the adapter (success) or swept (throw/null). create_instance and
    // project-load do the same inline; the reload paths used to skip it, so a
    // plugin allocating pool images in its ctor leaked them on every hot-reload.
    std::shared_ptr<CAbiInstanceAdapter> make_adapter_guarded_(
            PluginInfo& pi, const std::string& plugin_name,
            const std::string& inst_name, int max_concurrency) {
        if (!pi.c_factory) return nullptr;
        xi_host_api& host = default_host_api();
        ImagePoolOwnerScope owner;   // sweeps the ctor's images unless adopted below
        void* raw = nullptr;
        try {
            raw = owner.run_factory([&] { return pi.c_factory(&host, inst_name.c_str()); });
        } catch (...) { raw = nullptr; }
        if (!raw) return nullptr;    // owner dtor sweeps
        auto inst = std::make_shared<CAbiInstanceAdapter>(
            inst_name, plugin_name, pi.handle, raw, pi.reentrant, max_concurrency);
        inst->adopt_owner_id(owner.release());
        return inst;
    }

    // Phase 1 of a reload: snapshot every instance's def, then destroy them and
    // FreeLibrary the plugin's old DLL. The instance dtor runs xi_plugin_destroy
    // (joins worker threads / frees a CUDA context) BEFORE we unload, and freeing
    // the DLL releases the on-disk file lock so a rebuild (cl.exe-versioned or
    // cmake fixed-name) can overwrite it. Returns the snapshots + the old module
    // base (for the post-reload stale-module fail-check). mu_ MUST be held.
    std::vector<PendingInstance> detach_plugin_instances_locked_(
            const std::string& plugin_name, HMODULE* old_base_out) {
        std::vector<PendingInstance> pending;
        for (auto& [iname, ii] : project_.instances) {
            if (ii.plugin_name != plugin_name) continue;
            PendingInstance p; p.name = iname; p.folder = ii.folder_path;
            p.max_concurrency = ii.max_concurrency;
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
    bool reattach_plugin_from_dll_locked_(const std::string& plugin_name,
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
        HMODULE h = LoadLibraryExA(new_dll_path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
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
        pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
            GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
        if (!pi.c_factory) {
            if (err) *err = "factory '" + pi.factory_symbol + "' not exported in "
                            "rebuilt DLL — instances for this plugin are gone; "
                            "reopen the project to recover";
            return false;
        }

        for (auto& p : pending) {
            auto inst = make_adapter_guarded_(pi, plugin_name, p.name, p.max_concurrency);
            if (!inst) continue;
            if (!p.def_json.empty()) inst->set_def(p.def_json);
            project_.instances[p.name].instance = inst;
            InstanceRegistry::instance().add(inst);
        }
        stamp_loaded_dll_(pi, new_dll_path);   // refresh change-gate
        if (stale_module) {
            if (err) *err = "DLL did not unload (same module base after reload) — "
                            "an instance likely left a worker thread or GPU context "
                            "alive; NEW code is NOT active";
            return false;
        }
        return true;
    }

    int compile_project_plugins_locked(const std::string& project_folder) {
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
    int compile_plugin_folders_locked_(const std::vector<std::filesystem::directory_entry>& folders) {
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
                {
                    auto mpath = entry.path() / "plugin.json";
                    if (std::filesystem::exists(mpath)) {
                        std::ifstream mf(mpath.string());
                        std::stringstream ms; ms << mf.rdbuf();
                        std::string mc = ms.str();
                        prebuilt = json_flag_true(mc, "prebuilt") ||
                                   (extract_string(mc, "build").value_or("") == "cmake");
                        if (auto d = extract_string(mc, "dll")) want_dll = *d;
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
                            if (stub.name.empty()) stub.name = pname;
                            if (stub.dll_name.empty()) stub.dll_name = want_dll;
                            if (stub.factory_symbol.empty()) stub.factory_symbol = "xi_plugin_create";
                            stub.prebuilt = true;
                            stub.description = "Project plugin (cmake, not built): " + pname;
                            plugins_[pname] = std::move(stub);
                            project_plugin_origin_[pname] = entry.path().string();
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
                // (e.g., older DLL still loaded from a previous open).
                auto prev = plugins_.find(pname);
                if (prev != plugins_.end() && prev->second.handle) {
                    FreeLibrary(prev->second.handle);
                    prev->second.handle = nullptr;
                    prev->second.c_factory = nullptr;
                }

                PluginInfo pi;
                pi.name           = pname;
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
                    if (auto f = extract_string(mc, "factory"))     pi.factory_symbol = *f;
                    pi.has_ui = (mc.find("\"has_ui\":true") != std::string::npos) ||
                                (mc.find("\"has_ui\": true") != std::string::npos);
                    pi.reentrant = json_flag_true(mc, "reentrant") ||
                                   json_flag_true(mc, "thread_safe");  // documented alias
                    pi.json_fallback = json_flag_true(mc, "json_fallback");
                    if (pi.has_ui) pi.ui_path = (entry.path() / "ui").string();
                    std::string mblock;
                    if (detail_find_key(mc, "manifest", mblock)) pi.manifest_json = std::move(mblock);
                }
                // Load the freshly built DLL up-front. Project plugins
                // skip the cert/baseline gate — they are inside the
                // user's own project, not third-party code, so we trust
                // the source. (Export will run cert.)
                auto dll_path = std::filesystem::path(pi.folder_path) / pi.dll_name;
                // LoadLibraryEx with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR so the
                // plugin's OWN folder is searched for its dependency DLLs — a
                // plugin can ship extra .dll deps right next to its plugin DLL.
                // DEFAULT_DIRS keeps the app dir (where OpenCV/turbojpeg/IPP are
                // deployed) + System32 + AddDllDirectory dirs in the search set;
                // it deliberately drops CWD/PATH (avoids accidental hijack).
                // NOTE: same-named DLLs still collide across plugins — Windows
                // keeps one module per base name per process (see adding-a-plugin.md).
                // TODO(linux): dlopen resolves deps via RPATH/$ORIGIN + LD_LIBRARY_PATH;
                // build plugin .so with -Wl,-rpath,$ORIGIN for the same "deps beside me".
                pi.handle = LoadLibraryExA(dll_path.string().c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
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
                plugins_[pi.name] = std::move(pi);
                project_plugin_origin_[pname] = entry.path().string();
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

public:
    // Hot-rebuild one project plugin and re-instantiate every instance
    // using it, preserving each instance's saved def. Used by the
    // extension's file watcher: edit plugin .cpp → save → backend
    // recompiles + reloads → next trigger uses the new code, no
    // backend restart needed.
    //
    // Returns: ok flag, build log, list of instance names that were
    // re-instantiated. On compile failure the OLD DLL stays loaded so
    // running inspection isn't disrupted.
    struct RecompileResult {
        bool                     ok = false;
        std::string              build_log;
        std::vector<xi::script::Diagnostic> diagnostics;
        std::vector<std::string> reattached_instances;
        std::string              error;
    };
    RecompileResult recompile_project_plugin(const std::string& plugin_name) {
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
        };
        std::vector<Pending> pending;
        for (auto& [iname, ii] : project_.instances) {
            if (ii.plugin_name != plugin_name) continue;
            Pending p;
            p.name   = iname;
            p.folder = ii.folder_path;
            p.max_concurrency = ii.max_concurrency;
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
                auto inst = make_adapter_guarded_(pi_old, plugin_name, p.name, p.max_concurrency);
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
                    auto inst = make_adapter_guarded_(pi_it->second, plugin_name, p.name, p.max_concurrency);
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
        // Search the plugin's own folder for its dependency DLLs (see the
        // matching call in load_project_plugins above for the rationale).
        pi.handle = LoadLibraryExA(cres.dll_path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
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
            auto inst = make_adapter_guarded_(pi, plugin_name, p.name, p.max_concurrency);
            if (!inst) continue;
            if (!p.def_json.empty()) inst->set_def(p.def_json);
            project_.instances[p.name].instance = inst;
            InstanceRegistry::instance().add(inst);
            r.reattached_instances.push_back(p.name);
        }
        r.ok = true;
        return r;
    }

    struct PluginRebuildReport {
        // status: "rebuilt" | "unchanged" | "failed"
        struct Item { std::string name, status, detail; };
        std::vector<Item> items;
    };

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
    PluginRebuildReport rebuild_cmake_plugins(const std::string& cmake_exe,
                                              const std::string& config,
                                              const std::vector<std::string>& only = {}) {
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
    using ExportResult = xi::PluginExportResult;
    ExportResult export_project_plugin(const std::string& plugin_name,
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
    bool is_project_plugin(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        return project_plugin_origin_.count(name) > 0;
    }

    // Load a plugin's DLL and resolve the factory function.
    //
    // Plugins are trusted: the DLL is loaded straight through after an ABI
    // compatibility check (no baseline cert gate — removed 2026-06).
    // err (optional): on failure, filled with a human-readable reason so callers
    // (the create_instance handler) can surface WHY instead of a generic message.
    bool load_plugin(const std::string& name, std::string* err = nullptr) {
        auto fail = [&](std::string msg) { if (err) *err = std::move(msg); return false; };
        std::lock_guard<std::mutex> lk(mu_);
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

        pi.handle = LoadLibraryA(dll_path.string().c_str());
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
    std::vector<PluginInfo> list_plugins() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<PluginInfo> out;
        for (auto& [k, v] : plugins_) out.push_back(v);
        return out;
    }

    PluginInfo* find_plugin(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = plugins_.find(name);
        return it == plugins_.end() ? nullptr : &it->second;
    }

    // --- Project management ---

    bool create_project(const std::string& folder, const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        std::filesystem::create_directories(folder);
        std::filesystem::create_directories(std::filesystem::path(folder) / "instances");

        // Drop any stale folder/registry entries from a previous project
        for (auto& [k, v] : project_.instances) {
            InstanceRegistry::instance().remove(k);
            InstanceFolderRegistry::instance().clear(k);
        }
        project_.name = name;
        project_.folder_path = folder;
        project_.script_path = (std::filesystem::path(folder) / "inspect.cpp").string();
        project_.instances.clear();
        // New projects start with the local ./plugins root visible + an empty
        // plugins map; declare each plugin (or add it from the Plugin Browser).
        project_.plugin_dirs = {"./plugins"};
        project_.plugins.clear();

        // Write initial project.json
        save_project_locked();

        // Create a starter inspect.cpp if it doesn't exist
        if (!std::filesystem::exists(project_.script_path)) {
            std::string body =
                "// " + name + " — inspection script\n"
                "#include <xi/xi.hpp>\n"
                "// xi.hpp pulls in OpenCV. For image ops call cv:: directly\n"
                "// with xi::Image::as_cv_mat() / create_in_pool().\n\n"
                "XI_SCRIPT_EXPORT\n"
                "void xi_inspect_entry(int frame) {\n"
                "    // TODO: add inspection logic\n"
                "}\n";
            xi::atomic_write(project_.script_path, body);
        }
        return true;
    }

    void close_project() {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [k, v] : project_.instances) {
            InstanceRegistry::instance().remove(k);
            InstanceFolderRegistry::instance().clear(k);
        }
        // Destroy instances FIRST — same constraint as open_project (line
        // ~995): CAbiInstanceAdapter's destructor calls the plugin's
        // destroy_fn, which lives in the project plugin's DLL. FreeLibrary
        // before the adapter dies leaves the destructor calling a dangling
        // function pointer and the backend SEGVs. Clearing project_
        // instances explicitly here lets us choose the order; the
        // subsequent project_ = ProjectInfo{} would only destroy them
        // after the FreeLibrary calls below if we relied on aggregate
        // assignment.
        // FL r8 P1 reproducer: harness_open_close_cycle.py iter 0.
        project_.instances.clear();
        inst_state_.clear();   // host-tracked lifecycle state dies with the instances
        // Now safe to drop the previous project's plugins — adapters are
        // gone, no live destroy_fn callers remain.
        for (auto& [pname, _] : project_plugin_origin_) {
            auto it = plugins_.find(pname);
            if (it != plugins_.end()) {
                if (it->second.handle) FreeLibrary(it->second.handle);
                plugins_.erase(it);
            }
        }
        project_plugin_origin_.clear();
        project_ = ProjectInfo{};
    }

    // ---- working copy (transactional edits at <project>/.xinsp_work) --------
    // Constants + the filesystem mechanics (seed/mirror/exclude/gitignore) live
    // in xi_working_copy.hpp; these aliases keep the references below terse. The
    // stateful transactional methods (open/commit/discard) stay here.
    static constexpr const char* kWorkingCopyDir = xi::wc::kWorkingCopyDir;
    static constexpr const char* kCommitMarker   = xi::wc::kCommitMarker;

    // The canonical project dir when a working copy is active; empty otherwise.
    const std::string& canonical_path() const { return canonical_path_; }
    bool has_working_copy() const { return !canonical_path_.empty(); }

    // Commit: mirror the working copy back onto the canonical project (adds +
    // overwrites + deletes removed files), so the on-disk project reflects every
    // edit made this session. No-op error if no working copy is active.
    bool commit_working_copy() {
        std::lock_guard<std::mutex> lk(mu_);
        if (canonical_path_.empty()) return false;
        // Journal the commit so an interruption (crash/power loss mid-mirror) is
        // detectable + recoverable. The scratch is never modified by the mirror,
        // so it stays a complete snapshot; mirror_tree_ is idempotent, so the
        // next open_project can roll a partial commit forward from it.
        std::error_code ec;
        std::filesystem::path marker = std::filesystem::path(canonical_path_) / kCommitMarker;
        // F4: the marker MUST be durable on disk before the mirror starts — a bare
        // ofstream leaves it in the OS cache, so a power loss mid-mirror could tear
        // the canonical with no marker to drive roll-forward. atomic_write flushes
        // (FlushFileBuffers + atomic rename). If even the marker can't be written,
        // abort rather than mirror without a journal.
        if (!xi::atomic_write(marker, std::string("commit in progress\n"))) {
            std::fprintf(stderr, "[xinsp2] working copy: commit-marker write failed — aborting commit\n");
            return false;
        }
        // If the mirror hit a disk error (full disk, read-only canonical, a locked
        // destination file), the canonical tree is torn. Do NOT remove the marker:
        // leaving it makes the next open_project roll the (intact) scratch forward
        // and heal the canonical. Report failure rather than a false "committed".
        if (!xi::wc::mirror_tree(project_.folder_path, canonical_path_)) {
            std::fprintf(stderr, "[xinsp2] working copy: commit mirror FAILED (disk error?) "
                         "— marker kept for roll-forward on next open\n");
            return false;
        }
        std::filesystem::remove(marker, ec);   // commit complete -> clear journal
        std::fprintf(stderr, "[xinsp2] working copy: committed to %s\n",
                     canonical_path_.c_str());
        return true;
    }

    // Discard: blow away the working copy and re-seed it from the canonical
    // project, then reopen. Returns false if no working copy is active.
    bool reopen_fresh_working_copy() {
        std::string canon;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (canonical_path_.empty()) return false;
            canon = canonical_path_;
        }
        // close_project()/open_project() each take mu_ — don't hold it here.
        close_project();
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path(canon) / kWorkingCopyDir, ec);
        return open_project(canon, /*working_copy=*/true);   // re-seeds from canonical
    }

    bool open_project(const std::string& folder_arg, bool working_copy = false) {
        std::lock_guard<std::mutex> lk(mu_);

        // Roll forward an interrupted working-copy commit before touching
        // anything: if the canonical carries the commit-pending marker, a prior
        // commit was cut short (crash/power loss) and the canonical tree may be
        // torn. The scratch is a complete, untouched snapshot, so re-running the
        // (idempotent) mirror finishes the commit and heals the canonical.
        {
            std::filesystem::path canon = folder_arg;
            std::filesystem::path marker = canon / kCommitMarker;
            std::filesystem::path scratch = canon / kWorkingCopyDir;
            std::error_code ec;
            if (std::filesystem::exists(marker) &&
                std::filesystem::exists(scratch / "project.json")) {
                std::fprintf(stderr, "[xinsp2] working copy: completing interrupted "
                             "commit from %s (canonical may be torn)\n",
                             scratch.string().c_str());
                // Only clear the journal marker if the roll-forward actually
                // succeeded. If the disk error that interrupted the original commit
                // persists (read-only / full), mirror_tree returns false and we KEEP
                // the marker + scratch so the next open retries — removing it here
                // would strand a torn canonical with no record to heal it.
                if (xi::wc::mirror_tree(scratch, canon)) {
                    std::filesystem::remove(marker, ec);
                } else {
                    std::fprintf(stderr, "[xinsp2] working copy: roll-forward FAILED "
                                 "(disk error?) — keeping commit marker to retry on next open\n");
                }
            }
        }

        // Working-copy mode: operate on a scratch copy at <project>/.xinsp_work
        // so edits never touch the canonical project until an explicit commit
        // (and survive a backend crash — the scratch is on disk). Resume an
        // existing scratch (crash recovery / unsaved session); otherwise seed it
        // from the canonical project. `folder` is then rebased to the scratch so
        // ALL downstream logic (compile, instances, saves) uses the working copy.
        std::string folder = folder_arg;
        canonical_path_.clear();
        if (working_copy) {
            std::filesystem::path canon = folder_arg;
            if (!std::filesystem::exists(canon / "project.json")) return false;
            std::filesystem::path scratch = canon / kWorkingCopyDir;
            if (!std::filesystem::exists(scratch / "project.json")) {
                std::error_code ec;
                std::filesystem::remove_all(scratch, ec);   // clear any partial seed
                xi::wc::copy_tree_excluding(canon, scratch);
                std::fprintf(stderr, "[xinsp2] working copy: seeded %s from project\n",
                             scratch.string().c_str());
            } else {
                std::fprintf(stderr, "[xinsp2] working copy: resuming existing %s\n",
                             scratch.string().c_str());
            }
            xi::wc::ensure_gitignore(canon, std::string(kWorkingCopyDir) + "/");
            canonical_path_ = canon.string();
            folder = scratch.string();
        }

        auto pj = std::filesystem::path(folder) / "project.json";
        if (!std::filesystem::exists(pj)) return false;

        // Unregister old instances from the global registries.
        for (auto& [k, v] : project_.instances) {
            InstanceRegistry::instance().remove(k);
            InstanceFolderRegistry::instance().clear(k);
        }
        // Destroy old instances FIRST — CAbiInstanceAdapter's destructor
        // calls its plugin's destroy_fn, which lives in the project
        // plugin's DLL. If we FreeLibrary the DLL before the adapter
        // dies (the prior order did), the destructor calls a dangling
        // function pointer and SEGVs the backend on a reopen.
        project_.instances.clear();

        // Now safe to drop the previous project's plugins — adapters
        // are gone, no live destroy_fn callers remain.
        for (auto& [pname, _] : project_plugin_origin_) {
            auto it = plugins_.find(pname);
            if (it != plugins_.end()) {
                if (it->second.handle) FreeLibrary(it->second.handle);
                plugins_.erase(it);
            }
        }
        project_plugin_origin_.clear();
        project_.folder_path = folder;

        // Parse project.json
        std::ifstream f(pj.string());
        std::stringstream ss;
        ss << f.rdbuf();
        std::string content = ss.str();

        // Minimal parsing — extract name, script, plugins, instances
        auto name_opt = extract_string(content, "name");
        if (name_opt) project_.name = *name_opt;
        auto script_opt = extract_string(content, "script");
        if (script_opt) project_.script_path = (std::filesystem::path(folder) / *script_opt).string();
        else            project_.script_path = (std::filesystem::path(folder) / "inspect.cpp").string();

        // Parse trigger_policy block (optional; older project.json files
        // have none and we default to Any). Use a JSON parser instead of
        // substring search — the previous string-scan version assumed
        // `"required":[` with no whitespace and silently dropped the
        // list when a tool / human formatted the JSON with a space
        // after the colon (Python's json.dumps default).
        project_.dispatch_threads  = 1;
        project_.queue_depth       = 100;
        project_.overflow          = "drop_oldest";
        project_.result_order      = "completion";
        project_.groups.clear();
        project_.default_group.clear();
        project_.runtime_priority.clear();
        project_.runtime_timer_fps = -1;
        // Reset surfaced warnings here (before the project.json parse) so group
        // parse warnings, compile failures, and bad instances all accumulate.
        last_open_warnings_.clear();
        // Was the top-level project.json itself well-formed? A malformed file
        // (truncated / garbage) used to load "successfully" with all defaults and
        // no signal to the user — surface it as an open warning below.
        bool project_json_malformed = false;
        // External plugin coordinates (resolved after project-local plugins build):
        // plugin_dirs = ordered search roots; plugins = { label: { path } } refs.
        std::vector<std::string> proj_plugin_dirs;
        std::vector<ProjectInfo::PluginRef> proj_plugin_refs;
        // Project-level DEFAULT for a `plugins` entry that omits its own `compile`
        // flag; per-entry `compile` (parsed below) overrides it. Default off.
        bool proj_plugin_dirs_compile = json_flag_true(content, "plugin_dirs_compile");
        yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (root) {
            if (yyjson_val* pd = yyjson_obj_get(root, "plugin_dirs"); pd && yyjson_is_arr(pd)) {
                size_t _pi, _pn; yyjson_val* it;
                yyjson_arr_foreach(pd, _pi, _pn, it)
                    if (yyjson_is_str(it) && yyjson_get_str(it)) proj_plugin_dirs.push_back(yyjson_get_str(it));
            }
            if (yyjson_val* pl = yyjson_obj_get(root, "plugins"); pl && yyjson_is_obj(pl)) {
                size_t _pi, _pn; yyjson_val *k, *v;
                yyjson_obj_foreach(pl, _pi, _pn, k, v) {
                    if (!yyjson_is_obj(v)) continue;
                    const char* label = yyjson_get_str(k);
                    yyjson_val* pathv = yyjson_obj_get(v, "path");
                    if (label && pathv && yyjson_is_str(pathv) && yyjson_get_str(pathv)) {
                        bool compile = proj_plugin_dirs_compile;   // project default
                        if (yyjson_val* cv = yyjson_obj_get(v, "compile"); cv && yyjson_is_bool(cv))
                            compile = yyjson_get_bool(cv);          // per-plugin override
                        proj_plugin_refs.push_back({label, yyjson_get_str(pathv), compile});
                    }
                }
            }
            // (trigger_policy removed in the ABI-v6 dispatch cleanup — multi-cam
            // sync is now a gathering plugin, so the bus does no correlation. A
            // legacy trigger_policy block in project.json is simply ignored.)
            // runtime block — operational knobs (also live-settable). process_priority
            // applied on open; timer_fps seeds the live timer rate.
            if (yyjson_val* rt = yyjson_obj_get(root, "runtime"); rt && yyjson_is_obj(rt)) {
                if (yyjson_val* k = yyjson_obj_get(rt, "process_priority"); k && yyjson_is_str(k) && yyjson_get_str(k))
                    project_.runtime_priority = yyjson_get_str(k);
                if (yyjson_val* k = yyjson_obj_get(rt, "timer_fps"); k && yyjson_is_num(k))
                    project_.runtime_timer_fps = (int)yyjson_get_num(k);
            }
            // parallelism block.
            if (yyjson_val* par = yyjson_obj_get(root, "parallelism");
                par && yyjson_is_obj(par)) {
                if (yyjson_val* k = yyjson_obj_get(par, "dispatch_threads");
                    k && yyjson_is_num(k)) {
                    int n = (int)yyjson_get_num(k);
                    if (n < 1) n = 1;
                    if (n > 32) n = 32;  // sanity cap
                    project_.dispatch_threads = n;
                }
                if (yyjson_val* k = yyjson_obj_get(par, "queue_depth");
                    k && yyjson_is_num(k)) {
                    int n = (int)yyjson_get_num(k);
                    if (n < 1)     n = 1;
                    if (n > 10000) n = 10000;
                    project_.queue_depth = n;
                }
                if (yyjson_val* k = yyjson_obj_get(par, "overflow");
                    k && yyjson_is_str(k) && yyjson_get_str(k)) {
                    std::string s = yyjson_get_str(k);
                    if (s == "drop_oldest" || s == "drop_newest" || s == "block") {
                        project_.overflow = s;
                    } else {
                        std::fprintf(stderr,
                            "[xinsp2] project.json parallelism.overflow "
                            "unknown value '%s' — using drop_oldest\n",
                            s.c_str());
                    }
                }
                if (yyjson_val* k = yyjson_obj_get(par, "result_order");
                    k && yyjson_is_str(k) && yyjson_get_str(k)) {
                    std::string s = yyjson_get_str(k);
                    if (s == "completion" || s == "arrival") {
                        project_.result_order = s;
                    } else {
                        std::fprintf(stderr,
                            "[xinsp2] project.json parallelism.result_order "
                            "unknown value '%s' — using completion\n",
                            s.c_str());
                    }
                }
                // parallelism.groups + default_group (optional; empty = legacy pool)
                if (yyjson_val* arr = yyjson_obj_get(par, "groups"); arr && yyjson_is_arr(arr)) {
                    auto warn = [&](const std::string& who, const std::string& msg) {
                        last_open_warnings_.push_back({who, "", msg});
                        std::fprintf(stderr, "[xinsp2] parallelism.groups: %s — %s\n", who.c_str(), msg.c_str());
                    };
                    size_t _gi, _gn; yyjson_val* g;
                    yyjson_arr_foreach(arr, _gi, _gn, g) {
                        if (!yyjson_is_obj(g)) continue;
                        ProjectInfo::DispatchGroup grp;
                        if (yyjson_val* k = yyjson_obj_get(g, "name"); k && yyjson_is_str(k) && yyjson_get_str(k)) grp.name = yyjson_get_str(k);
                        if (grp.name.empty()) { warn("(unnamed)", "group missing 'name' — skipped"); continue; }
                        if (project_.find_group(grp.name)) { warn(grp.name, "duplicate group name — skipped"); continue; }  // #7
                        if (yyjson_val* k = yyjson_obj_get(g, "max_parallel"); k && yyjson_is_num(k))
                            grp.max_parallel = std::min(32, std::max(1, (int)yyjson_get_num(k)));   // #4 clamp [1,32]
                        if (yyjson_val* k = yyjson_obj_get(g, "thread_priority"); k && yyjson_is_str(k) && yyjson_get_str(k)) {
                            grp.thread_priority = yyjson_get_str(k);
                            if (grp.thread_priority != "high" && grp.thread_priority != "normal" && grp.thread_priority != "low") {
                                warn(grp.name, "unknown thread_priority '" + grp.thread_priority + "' — using normal");
                                grp.thread_priority = "normal";
                            }
                        }
                        if (yyjson_val* k = yyjson_obj_get(g, "queue_depth"); k && yyjson_is_num(k))
                            grp.queue_depth = std::min(10000, std::max(1, (int)yyjson_get_num(k)));
                        if (yyjson_val* k = yyjson_obj_get(g, "overflow"); k && yyjson_is_str(k) && yyjson_get_str(k)) {
                            grp.overflow = yyjson_get_str(k);
                            if (grp.overflow != "drop_oldest" && grp.overflow != "drop_newest" && grp.overflow != "block") {
                                warn(grp.name, "unknown overflow '" + grp.overflow + "' — using drop_oldest");
                                grp.overflow = "drop_oldest";
                            }
                        }
                        if (yyjson_val* k = yyjson_obj_get(g, "result_order"); k && yyjson_is_str(k) && yyjson_get_str(k)) {
                            grp.result_order = yyjson_get_str(k);
                            if (grp.result_order != "completion" && grp.result_order != "arrival") {
                                warn(grp.name, "unknown result_order '" + grp.result_order + "' — using completion");
                                grp.result_order = "completion";
                            }
                        }
                        if (yyjson_val* k = yyjson_obj_get(g, "min_interval_ms"); k && yyjson_is_num(k))
                            grp.min_interval_ms = std::min(3600000, std::max(0, (int)yyjson_get_num(k)));
                        // cpu_affinity: flat [0,1,..] = one shared mask; nested
                        // [[..],[..]] = per-worker masks. Empty/invalid → unbound.
                        if (yyjson_val* k = yyjson_obj_get(g, "cpu_affinity"); k && yyjson_is_arr(k)) {
                            auto parse_set = [&](yyjson_val* arr) {
                                std::vector<int> s;
                                size_t _ei, _en; yyjson_val* e;
                                yyjson_arr_foreach(arr, _ei, _en, e) {
                                    if (!yyjson_is_num(e)) continue;
                                    int c = (int)yyjson_get_num(e);
                                    if (c >= 0 && c < 1024) s.push_back(c);
                                    else warn(grp.name, "cpu_affinity core " + std::to_string(c) + " out of range — ignored");
                                }
                                return s;
                            };
                            yyjson_val* first = yyjson_arr_get(k, 0);
                            if (first && yyjson_is_arr(first)) {          // nested: per-worker
                                size_t _ri, _rn; yyjson_val* row;
                                yyjson_arr_foreach(k, _ri, _rn, row)
                                    if (yyjson_is_arr(row)) { auto s = parse_set(row); if (!s.empty()) grp.cpu_affinity.push_back(std::move(s)); }
                            } else {                                       // flat: one shared mask
                                auto s = parse_set(k);
                                if (!s.empty()) grp.cpu_affinity.push_back(std::move(s));
                            }
                        }
                        project_.groups.push_back(std::move(grp));
                    }
                    if (yyjson_val* k = yyjson_obj_get(par, "default_group"); k && yyjson_is_str(k) && yyjson_get_str(k))
                        project_.default_group = yyjson_get_str(k);
                    if (project_.default_group.empty() && !project_.groups.empty())
                        project_.default_group = project_.groups.front().name;
                    if (!project_.default_group.empty() && !project_.find_group(project_.default_group))  // #6
                        warn(project_.default_group, "default_group names no declared group — falling back to '" +
                             (project_.groups.empty() ? std::string("(none)") : project_.groups.front().name) + "'");
                }
            }
            yyjson_doc_free(doc);
        } else {
            yyjson_doc_free(doc);
            project_json_malformed = true;
        }
        // Compile project-local plugins BEFORE instances are loaded — the
        // instance loop below resolves plugin name → loaded DLL, and we
        // want project plugins to win over global ones with the same name.
        // (last_open_warnings_ was reset at the top of open_project so group-parse
        // warnings, compile failures, and bad instances all accumulate together.)
        if (project_json_malformed) {
            last_open_warnings_.push_back({"", "",
                "project.json is not valid JSON - loaded with defaults "
                "(check for a syntax error or truncation)"});
            std::fprintf(stderr, "[xinsp2] project.json malformed - loaded with defaults\n");
        }
        // Declarative plugin model: EVERY plugin (local + external) comes from the
        // project.json `plugins` declarations, resolved against `plugin_dirs`. There
        // is NO auto-discovery of <project>/plugins/* — a folder there does nothing
        // until it's listed in `plugins`. `plugin_dirs` falls back to ["./plugins"]
        // only when unset; set it and you get exactly your roots (re-add "./plugins"
        // yourself if you still want it). Each `plugins` entry's `compile` decides
        // whether its resolved folder is cl.exe-compiled / treated as a trusted
        // project plugin (recompile/rebuild-able) or just registered.
        // Persist the declarations as-written (pre-fallback) so save_project_locked
        // round-trips them without baking in the implicit ./plugins.
        project_.plugin_dirs = proj_plugin_dirs;
        project_.plugins     = proj_plugin_refs;
        if (proj_plugin_dirs.empty()) proj_plugin_dirs.push_back("./plugins");
        resolve_external_project_plugins_locked_(folder, proj_plugin_dirs, proj_plugin_refs);

        // Scan instances/ subdirectories. A broken instance.json or a
        // factory that throws must NOT abort the whole project load —
        // record the failure in last_open_warnings_ and move on. The
        // user can read it via cmd:open_project_warnings and decide
        // whether to fix or delete the bad instance folder.
        // (last_open_warnings_ already cleared above before plugin compile.)
        auto inst_dir = std::filesystem::path(folder) / "instances";
        bool warned_iso_deprecated_ = false;
        if (std::filesystem::exists(inst_dir)) {
            for (auto& entry : std::filesystem::directory_iterator(inst_dir)) {
                if (!entry.is_directory()) continue;
                std::string inst_name = entry.path().filename().string();
                try {
                auto ij = entry.path() / "instance.json";
                if (!std::filesystem::exists(ij)) {
                    last_open_warnings_.push_back({inst_name, "", "missing instance.json"});
                    std::fprintf(stderr, "[xinsp2] skip instance '%s': missing instance.json\n",
                                 inst_name.c_str());
                    continue;
                }
                std::ifstream ijf(ij.string());
                std::stringstream iss;
                iss << ijf.rdbuf();
                std::string ic = iss.str();
                auto plugin = extract_string(ic, "plugin");
                if (!plugin) {
                    last_open_warnings_.push_back({inst_name, "", "instance.json missing 'plugin' field (or unparseable)"});
                    std::fprintf(stderr, "[xinsp2] skip instance '%s': no plugin field\n",
                                 inst_name.c_str());
                    continue;
                }

                InstanceInfo ii;
                ii.name = inst_name;
                ii.plugin_name = *plugin;
                ii.folder_path = entry.path().string();
                if (auto g = extract_string(ic, "group")) ii.group = *g;   // dispatch group for this source's triggers
                // Optional per-instance concurrency cap (reentrant plugins only;
                // 0/absent = unlimited). Parse JSON for the numeric field.
                if (yyjson_doc* idoc = yyjson_read(ic.c_str(), ic.size(), 0)) {
                    yyjson_val* iroot = yyjson_doc_get_root(idoc);
                    if (yyjson_val* k = iroot ? yyjson_obj_get(iroot, "max_concurrency") : nullptr;
                        k && yyjson_is_num(k) && yyjson_get_num(k) > 0) {
                        ii.max_concurrency = (int)yyjson_get_num(k);
                    }
                    yyjson_doc_free(idoc);
                }
                // Auto-load the plugin if not yet loaded
                auto pit = plugins_.find(*plugin);
                if (pit != plugins_.end() && !pit->second.c_factory) {
                    // Plugin discovered but not loaded — load it now
                    auto& pi2 = pit->second;
                    auto dll_path = std::filesystem::path(pi2.folder_path) / pi2.dll_name;
                    if (std::filesystem::exists(dll_path)) {
                        pi2.handle = LoadLibraryA(dll_path.string().c_str());
                        if (pi2.handle) {
                            // P0-D3: ABI compatibility check was missing
                            // on this code path. A stale plugin DLL built
                            // against a future ABI loaded silently and
                            // got called with the new ABI signatures —
                            // memory corruption risk. Mirror the load_plugin
                            // path's check.
                            std::string err;
                            if (!plugin_abi_compatible(pi2.handle, *plugin, pi2.json_fallback, &err)) {
                                FreeLibrary(pi2.handle);
                                pi2.handle = nullptr;
                                last_open_warnings_.push_back(
                                    {inst_name, *plugin, "plugin ABI mismatch: " + err});
                                std::fprintf(stderr,
                                    "[xinsp2] skip instance '%s': %s\n",
                                    inst_name.c_str(), err.c_str());
                                continue;
                            }
                            pi2.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                                GetProcAddress(pi2.handle, pi2.factory_symbol.c_str()));
                            // P0-D3 (cont.): if the factory symbol doesn't
                            // resolve, the DLL is loaded but unusable.
                            // Previously left in place with handle set
                            // and factory=null; subsequent open_project
                            // attempts found a stale entry. FreeLibrary
                            // and clear handle so the entry stays in a
                            // clean "not loaded" state.
                            if (!pi2.c_factory) {
                                FreeLibrary(pi2.handle);
                                pi2.handle = nullptr;
                                last_open_warnings_.push_back(
                                    {inst_name, *plugin,
                                     "plugin DLL has no factory symbol "
                                     + pi2.factory_symbol});
                                std::fprintf(stderr,
                                    "[xinsp2] skip instance '%s': no factory '%s'\n",
                                    inst_name.c_str(),
                                    pi2.factory_symbol.c_str());
                                continue;
                            }
                        }
                    }
                }
                if (pit != plugins_.end()) {
                    auto& pi = pit->second;
                    bool created = false;
                    // Same registration as create_instance — needed for project-load too.
                    InstanceFolderRegistry::instance().set(ii.name, ii.folder_path);

                    // Every instance runs in-process: plugins are loaded
                    // into the backend and called directly (zero-copy via
                    // pointers into the host ImagePool).
                    //
                    // Architecture pivot (2026-05): process isolation + SHM
                    // were removed in favour of a single in-process compute
                    // core under a frontend supervisor. A dead plugin means
                    // a dead pipeline regardless of isolation, so per-plugin
                    // sandboxing bought nothing but the SHM / worker-process
                    // complexity. `isolation:"process"` (and "in_process")
                    // are accepted but ignored, with a one-time deprecation
                    // warning, so old project.json files keep loading.
                    if (auto iso = extract_string(ic, "isolation");
                        iso && *iso == "process" && !warned_iso_deprecated_) {
                        std::fprintf(stderr,
                            "[xinsp2] isolation:\"process\" is deprecated and ignored — "
                            "all plugins run in-process now (single compute core). "
                            "Remove the field from instance.json to silence this.\n");
                        warned_iso_deprecated_ = true;
                    }

                    if (!created && pi.c_factory) {
                        xi_host_api& host = default_host_api();
                        // ImagePoolOwnerScope tags the ctor's images and sweeps them
                        // automatically on a null return OR a throw (its dtor runs as
                        // the exception unwinds to the per-instance handler below) —
                        // the adapter never got built, so it can't sweep for us.
                        ImagePoolOwnerScope owner;
                        void* raw = owner.run_factory(
                            [&] { return pi.c_factory(&host, ii.name.c_str()); });
                        if (raw) {
                            auto adapter = std::make_shared<CAbiInstanceAdapter>(
                                ii.name, *plugin, pi.handle, raw, pi.reentrant, ii.max_concurrency);
                            // Hand the owner id to the adapter so subsequent process /
                            // exchange calls keep tagging into the same bucket.
                            adapter->adopt_owner_id(owner.release());
                            ii.instance = std::move(adapter);
                            created = true;
                        }
                    }
                    if (created && ii.instance) {
                        std::string cfg_val;
                        if (detail_find_key(ic, "config", cfg_val)) {
                            // FL r6 P2-3: validate instance.json.config against
                            // plugin.json.manifest.params before handing it to
                            // the plugin. Bad keys / out-of-range values still
                            // fall through to set_def() (which silently ignores
                            // unknown keys); the warnings here are how the user
                            // learns about a typo. Skipped if the plugin
                            // doesn't declare manifest.params (back-compat).
                            validate_config_against_manifest(
                                inst_name, *plugin, cfg_val,
                                pi.manifest_json, last_open_warnings_);
                            ii.instance->set_def(cfg_val);
                        }
                        InstanceRegistry::instance().add(ii.instance);
                    }
                    if (!created) {
                        // B-P1-2: factory failed → adapter wasn't created,
                        // but InstanceFolderRegistry::set ran a few lines
                        // up. Clear the folder-registry entry so a stale
                        // reference doesn't outlive this open_project.
                        InstanceFolderRegistry::instance().clear(inst_name);
                        last_open_warnings_.push_back(
                            {inst_name, *plugin, "factory returned null"});
                        std::fprintf(stderr,
                            "[xinsp2] skip instance '%s' (%s): factory returned null\n",
                            inst_name.c_str(), plugin->c_str());
                    } else {
                        // Only persist the InstanceInfo in project_.instances
                        // when we actually have a live adapter. Skip-bad
                        // entries shouldn't become "phantom" entries that
                        // close_project then iterates over.
                        project_.instances[ii.name] = std::move(ii);
                    }
                } else {
                    // B-P1-2: same gap as the factory-null branch above —
                    // InstanceFolderRegistry::set wasn't called yet here
                    // (we only set it after pit was found), but log+continue
                    // shape stays consistent.
                    last_open_warnings_.push_back(
                        {inst_name, *plugin, "plugin not loaded / not found"});
                    std::fprintf(stderr,
                        "[xinsp2] skip instance '%s': plugin '%s' not loaded\n",
                        inst_name.c_str(), plugin->c_str());
                }
                } catch (const std::exception& e) {
                    // B-P1-1: catch ran AFTER InstanceFolderRegistry::set
                    // and (potentially) InstanceRegistry::add inside the
                    // try block, but didn't undo either. Stale entries
                    // leaked until the NEXT open_project's bulk clear.
                    // Symmetric cleanup here:
                    InstanceFolderRegistry::instance().clear(inst_name);
                    InstanceRegistry::instance().remove(inst_name);
                    last_open_warnings_.push_back(
                        {inst_name, "", std::string("exception: ") + e.what()});
                    std::fprintf(stderr,
                        "[xinsp2] skip instance '%s': %s\n",
                        inst_name.c_str(), e.what());
                } catch (...) {
                    InstanceFolderRegistry::instance().clear(inst_name);
                    InstanceRegistry::instance().remove(inst_name);
                    last_open_warnings_.push_back(
                        {inst_name, "", "unknown exception during load"});
                    std::fprintf(stderr,
                        "[xinsp2] skip instance '%s': unknown exception\n",
                        inst_name.c_str());
                }
            }
        }
        return true;
    }

    // An instance name becomes a folder under <project>/instances/, so it MUST be
    // a single safe path segment — never a path. Without this an absolute path or
    // a `..` in the name escapes the project folder (create writes / remove
    // recursively deletes / rename moves an attacker- or typo-chosen directory).
    // Allow only identifier-ish chars; reject separators, drive colon, and `..`.
    static bool is_valid_instance_name(const std::string& n) {
        if (n.empty() || n.size() > 128) return false;
        if (n == "." || n.find("..") != std::string::npos) return false;
        for (unsigned char c : n)
            if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
        return true;
    }

    // Create a new instance of a plugin inside the current project.
    InstanceInfo* create_instance(const std::string& instance_name,
                                   const std::string& plugin_name,
                                   std::string* err = nullptr) {
        auto fail = [&](std::string msg) -> InstanceInfo* { if (err) *err = std::move(msg); return nullptr; };
        std::lock_guard<std::mutex> lk(mu_);
        if (project_.folder_path.empty())
            return fail("no project open — open_project before creating an instance");
        if (!is_valid_instance_name(instance_name))
            return fail("invalid instance name '" + instance_name +
                        "' — use letters/digits/_/-/. only (no path separators or '..')");

        auto pit = plugins_.find(plugin_name);
        if (pit == plugins_.end())
            return fail("plugin '" + plugin_name + "' not loaded");
        auto& pi = pit->second;
        if (!pi.c_factory)
            return fail("plugin '" + plugin_name + "' has no factory (load_plugin failed?)");

        auto inst_folder = std::filesystem::path(project_.folder_path) / "instances" / instance_name;
        // Use the error_code overload: the throwing one would propagate a
        // filesystem_error past the (un-try/catch'd) WS command loop and
        // terminate the whole backend on a benign environment failure (path too
        // long, instances/ read-only, disk full). Report it as a command error.
        {
            std::error_code ec;
            std::filesystem::create_directories(inst_folder, ec);
            if (ec)
                return fail("cannot create instance folder '" + inst_folder.string() +
                            "': " + ec.message());
        }

        InstanceInfo ii;
        ii.name = instance_name;
        ii.plugin_name = plugin_name;
        ii.folder_path = inst_folder.string();

        // Register the folder BEFORE the factory runs so the plugin can
        // call host->instance_folder() from inside its constructor.
        InstanceFolderRegistry::instance().set(instance_name, ii.folder_path);

        ImagePoolOwnerId created_owner = 0;   // for a later sweep if a save fails
        {
            // C ABI — create via host API. ImagePoolOwnerScope tags every image the
            // ctor allocates and AUTO-SWEEPS them on any early return below (throw /
            // null) — the leak-on-failed-ctor footgun is now structural, not manual.
            xi_host_api& host = default_host_api();
            ImagePoolOwnerScope owner;
            created_owner = owner.id();   // for the post-adopt save-failure sweep
            void* raw = nullptr;
            try {
                raw = owner.run_factory([&] { return pi.c_factory(&host, instance_name.c_str()); });
            } catch (const std::exception& e) {
                // A throwing ctor must not terminate the backend (the WS command
                // loop has no outer catch). owner sweeps the half-init handles; we
                // still clear the folder registry and report it as a command error.
                InstanceFolderRegistry::instance().clear(instance_name);
                return fail("plugin '" + plugin_name + "' factory threw: " + e.what());
            } catch (...) {
                InstanceFolderRegistry::instance().clear(instance_name);
                return fail("plugin '" + plugin_name + "' factory threw a non-standard exception");
            }
            if (!raw) {
                InstanceFolderRegistry::instance().clear(instance_name);
                return fail("plugin '" + plugin_name + "' factory returned null "
                            "(constructor failed/rejected the config)");
            }
            auto adapter = std::make_shared<CAbiInstanceAdapter>(
                instance_name, plugin_name, pi.handle, raw, pi.reentrant, /*max_concurrency=*/0);
            adapter->adopt_owner_id(owner.release());   // adapter owns the sweep now
            ii.instance = std::move(adapter);
        }
        InstanceRegistry::instance().add(ii.instance);

        // Save instance.json. If the write fails (disk full / read-only) don't
        // commit the instance: leaving it only in project.json (not on disk) would
        // make the next open_project silently lose the user's config. Sweep the
        // registries + folder and report the failure. (commit_working_copy applies
        // the same check on its pre-commit saves.)
        if (!save_instance_json(ii)) {
            InstanceRegistry::instance().remove(instance_name);
            InstanceFolderRegistry::instance().clear(instance_name);
            ImagePool::instance().release_all_for(created_owner);
            return fail("cannot write instance config for '" + instance_name +
                        "' (disk full / read-only?) — instance not created");
        }

        project_.instances[instance_name] = std::move(ii);
        set_state_locked_(instance_name, InstState::Created, "");   // born Created
        save_project_locked();
        return &project_.instances[instance_name];
    }

    // Save an instance's current config to its folder.
    // Returns false if the instance is unknown OR its on-disk write failed.
    bool save_instance(const std::string& instance_name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = project_.instances.find(instance_name);
        if (it == project_.instances.end()) return false;
        return save_instance_json(it->second);
    }

    // Remove an instance: destroys the runtime object + unregisters from
    // both registries. Optionally deletes the on-disk folder.
    bool remove_instance(const std::string& instance_name, bool delete_folder) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = project_.instances.find(instance_name);
        if (it == project_.instances.end()) return false;
        InstanceRegistry::instance().remove(instance_name);
        InstanceFolderRegistry::instance().clear(instance_name);
        inst_state_.erase(instance_name);   // lifecycle state dies with the instance
        std::string folder = it->second.folder_path;
        project_.instances.erase(it);
        if (delete_folder && !folder.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(folder, ec);
        } else if (!folder.empty()) {
            // "keep folder" must still PERSIST the removal. open_project discovers
            // instances by scanning instances/<name>/instance.json (the project.json
            // list is write-only / ignored on load), so leaving instance.json in
            // place would resurrect this instance on the next open. Move it aside to
            // instance.json.removed: the scan skips a folder with no instance.json,
            // yet the config + any assets stay on disk for manual recovery, which is
            // what "keep folder" means. (A later create_instance of the same name
            // writes a fresh instance.json, shadowing the tombstone.)
            std::error_code ec;
            auto ij = std::filesystem::path(folder) / "instance.json";
            if (std::filesystem::exists(ij, ec))
                std::filesystem::rename(ij, std::filesystem::path(folder) / "instance.json.removed", ec);
        }
        save_project_locked();
        return true;
    }

    // Result of rename_instance. The caller MUST distinguish Rejected (no mutation
    // happened — the old instance is untouched) from NotPersisted (the runtime +
    // on-disk folder were renamed to new_name, only the config save failed): on
    // NotPersisted the in-memory state IS the new name, so the caller still has to
    // migrate any side state keyed by name (e.g. g_inst_state) and report a
    // save-failed warning, NOT "rename failed" — reporting failure while the
    // runtime moved would desync name-keyed state.
    enum class RenameResult { Rejected, Ok, NotPersisted };

    // Rename an instance. Moves the on-disk folder and re-registers under the new
    // name. Rejected if the new name is invalid / in use / the instance is missing
    // (no side effects); Ok on full success; NotPersisted if the runtime renamed
    // but the config write failed.
    RenameResult rename_instance(const std::string& old_name, const std::string& new_name) {
        std::lock_guard<std::mutex> lk(mu_);
        if (old_name == new_name) return RenameResult::Ok;
        if (!is_valid_instance_name(new_name)) return RenameResult::Rejected;   // no path escape via the name
        auto it = project_.instances.find(old_name);
        if (it == project_.instances.end()) return RenameResult::Rejected;
        if (project_.instances.count(new_name)) return RenameResult::Rejected;

        // Validate everything that can fail WITHOUT side effects BEFORE touching
        // the filesystem / registries — a failure here used to leave the folder
        // already renamed with no rollback, orphaning the old instance.
        auto pit = plugins_.find(it->second.plugin_name);
        if (pit == plugins_.end()) return RenameResult::Rejected;
        auto& pi = pit->second;
        if (!pi.c_factory) return RenameResult::Rejected;

        auto old_folder = std::filesystem::path(it->second.folder_path);
        auto new_folder = old_folder.parent_path() / new_name;
        if (std::filesystem::exists(new_folder)) return RenameResult::Rejected;

        // Capture what we need from the old entry before any mutation.
        const std::string plugin_name      = it->second.plugin_name;
        const int         old_max_conc      = it->second.max_concurrency;
        const std::string old_group         = it->second.group;
        std::string saved_def;
        if (it->second.instance) saved_def = it->second.instance->get_def();

        // Move the folder. From here, any failure must roll this back so the OLD
        // instance (still fully registered, untouched below until success) keeps
        // working.
        std::error_code ec;
        std::filesystem::rename(old_folder, new_folder, ec);
        if (ec) return RenameResult::Rejected;
        auto rollback_folder = [&]() {
            std::error_code re; std::filesystem::rename(new_folder, old_folder, re);
        };

        // InstanceBase::name() is immutable, so recreate under the new name via
        // the factory; the old instance's config is carried via get_def→set_def.
        // Register the new folder BEFORE the factory so the ctor can read it.
        InstanceFolderRegistry::instance().set(new_name, new_folder.string());
        xi_host_api& host = default_host_api();
        // ImagePoolOwnerScope tags + auto-sweeps the ctor's images so a rename can't
        // orphan them at owner 0 (the leak this guards against). Mirrors
        // create_instance / project-load via the one RAII primitive.
        ImagePoolOwnerScope owner;
        void* raw = nullptr;
        try {
            raw = owner.run_factory([&] { return pi.c_factory(&host, new_name.c_str()); });
        } catch (...) {
            // A throwing ctor must not terminate the backend (no outer catch on
            // the WS loop). owner sweeps; undo the new-name registration + folder
            // move so the old instance stays intact.
            InstanceFolderRegistry::instance().clear(new_name);
            rollback_folder();
            return RenameResult::Rejected;
        }
        if (!raw) {
            InstanceFolderRegistry::instance().clear(new_name);
            rollback_folder();
            return RenameResult::Rejected;
        }

        // Factory succeeded — now commit: drop the old runtime entries and swap in
        // the new instance.
        InstanceRegistry::instance().remove(old_name);
        InstanceFolderRegistry::instance().clear(old_name);

        InstanceInfo ii;
        ii.name = new_name;
        ii.plugin_name = plugin_name;
        ii.folder_path = new_folder.string();
        // F2: carry over per-instance config that isn't in get_def() — else a
        // rename silently dropped the concurrency cap + dispatch group.
        ii.max_concurrency = old_max_conc;
        ii.group           = old_group;
        auto adapter = std::make_shared<CAbiInstanceAdapter>(
            new_name, plugin_name, pi.handle, raw, pi.reentrant, ii.max_concurrency);
        adapter->adopt_owner_id(owner.release());   // ctor images now belong to the live adapter
        ii.instance = std::move(adapter);
        if (!saved_def.empty()) ii.instance->set_def(saved_def);
        InstanceRegistry::instance().add(ii.instance);

        project_.instances.erase(old_name);
        project_.instances[new_name] = std::move(ii);
        // Carry the host-tracked lifecycle state across the rename in the SAME
        // locked op as the registries (old_name != new_name is guaranteed above).
        // This is why the WS handler no longer needs a separate rename_inst_state
        // call — the state can't drift from the instance set anymore.
        if (auto sit = inst_state_.find(old_name); sit != inst_state_.end()) {
            inst_state_[new_name] = sit->second;
            inst_state_.erase(sit);
        }
        // Surface a write failure: the runtime + folder are already renamed, so if
        // project.json/instance.json don't persist the new name the next open is
        // inconsistent. Return NotPersisted (NOT Rejected) so the caller still
        // reports it as a save failure rather than a rename failure (the rename
        // itself happened).
        bool saved = save_instance_json(project_.instances[new_name]);
        saved = save_project_locked() && saved;
        return saved ? RenameResult::Ok : RenameResult::NotPersisted;
    }

    ProjectInfo& project() { return project_; }

    // Thread-safe dispatch-group lookup for the bus sink, which runs on a source
    // plugin's emit thread concurrently with create/remove/rename_instance (those
    // mutate project_.instances under mu_). Reading project().instances unlocked
    // from the sink was a data race (find() vs erase() → UAF). Returns the
    // instance's group, or default_group when absent/unknown.
    std::string instance_group(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!name.empty()) {
            auto it = project_.instances.find(name);
            if (it != project_.instances.end() && !it->second.group.empty())
                return it->second.group;
        }
        return project_.default_group;
    }

    // ---- host-tracked instance lifecycle state -----------------------------
    // The state map is OWNED here, under the same mu_ as the instance set, so
    // create/remove/rename migrate it atomically (they hold mu_ and call
    // set_state_locked_ / erase / re-key inline). That removes the whole class of
    // "name-keyed side state drifted out of sync with the registries" bugs the
    // earlier rounds kept hitting (rename desync, rename(x,x) self-delete,
    // remove leaving a stale entry). Keyed by name to span backend + script-side
    // instances alike. set_*/get_* below are the public (locking) entry points
    // used by the WS handlers; CRUD methods use set_state_locked_ while holding mu_.
    void set_instance_state(const std::string& name, InstState s,
                            const std::string& err = std::string()) {
        std::lock_guard<std::mutex> lk(mu_);
        set_state_locked_(name, s, err);
    }
    // Returns true if a state has been recorded for `name` (filling out_*); false
    // if unknown (the caller may then fall back to "exists ⇒ Created").
    bool get_instance_state(const std::string& name, InstState& out_state,
                            std::string& out_err) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = inst_state_.find(name);
        if (it == inst_state_.end()) return false;
        out_state = it->second.state;
        out_err   = it->second.last_error;
        return true;
    }
    void clear_instance_states() {
        std::lock_guard<std::mutex> lk(mu_);
        inst_state_.clear();
    }

    std::string to_json() {
        std::lock_guard<std::mutex> lk(mu_);
        std::string out = "{\"name\":";
        pm_json_escape(out, project_.name);
        out += ",\"script\":";
        pm_json_escape(out, std::filesystem::path(project_.script_path).filename().string());
        out += ",\"instances\":[";
        int i = 0;
        for (auto& [k, v] : project_.instances) {
            if (i++) out += ",";
            out += "{\"name\":";  pm_json_escape(out, v.name);
            out += ",\"plugin\":"; pm_json_escape(out, v.plugin_name);
            out += "}";
        }
        out += "],\"plugins\":[";
        i = 0;
        for (auto& [k, v] : plugins_) {
            if (i++) out += ",";
            bool is_proj = project_plugin_origin_.count(v.name) > 0;
            out += "{\"name\":"; pm_json_escape(out, v.name);
            out += ",\"description\":"; pm_json_escape(out, v.description);
            out += ",\"has_ui\":" + std::string(v.has_ui ? "true" : "false");
            out += ",\"loaded\":" + std::string(v.handle ? "true" : "false");
            out += ",\"prebuilt\":" + std::string(v.prebuilt ? "true" : "false");
            // origin: "project" if compiled from <project>/plugins, else "global"
            out += ",\"origin\":\"" + std::string(is_proj ? "project" : "global") + "\"";
            if (is_proj) {
                out += ",\"source_dir\":";
                pm_json_escape(out, project_plugin_origin_[v.name]);
            }
            // Optional `manifest` block from plugin.json — passed through
            // verbatim. Clients (AI agents, doc tools) parse the body
            // themselves; the backend doesn't validate or reshape it.
            if (!v.manifest_json.empty()) {
                out += ",\"manifest\":" + v.manifest_json;
            }
            out += "}";
        }
        out += "]";
        // Working-copy status so the UI can target the scratch dir for editing
        // and surface a "save project" affordance.
        out += ",\"working_copy\":" + std::string(canonical_path_.empty() ? "false" : "true");
        if (!canonical_path_.empty()) {
            out += ",\"canonical_path\":"; pm_json_escape(out, canonical_path_);
            out += ",\"working_dir\":";    pm_json_escape(out, project_.folder_path);
        }
        out += "}";
        return out;
    }

    // Per-instance failure record from the most recent open_project: the open
    // succeeds even if individual instances fail (skip-bad-instance); callers
    // read these to surface a warning. OpenWarning lives in xi_project_model.hpp
    // (namespace xi) — used unqualified here, resolves to xi::OpenWarning.
    std::vector<OpenWarning> open_warnings() {
        std::lock_guard<std::mutex> lk(mu_);
        return last_open_warnings_;
    }

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
    CompileEnv  compile_env_;
    // Names of plugins that came from <project>/plugins/ rather than the
    // global plugins directory — flagged so the UI can label them and so
    // we don't re-scan their dll mtime against the global cert.
    std::unordered_map<std::string, std::string> project_plugin_origin_;
    // Canonical project dir when a working copy is active (project_.folder_path
    // then points at <canonical>/.xinsp_work). Empty = no working copy.
    std::string canonical_path_;

    // Shared host_api for in-process C-ABI plugin factory calls (image-pool host
    // + trigger hook). One process-wide instance: every factory site used to
    // declare its own byte-identical function-local static — this dedups them.
    // Cold path (instance create / recompile / rename), so the single shared
    // static is fine and costs nothing extra.
    static xi_host_api& default_host_api() {
        static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); return a; }();
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
    // included above). The transactional methods above call into them.

    // json_flag_true / extract_string / detail_find_key / parse_manifest moved
    // to xi_pm_parse.hpp; validate_config_against_manifest to xi_config_validate.hpp
    // (leaf; both included above). Called unqualified -> resolve in namespace xi.

    // Returns false if the on-disk write failed (disk full / read-only). The
    // in-memory project is unchanged and atomic_write left the prior file intact
    // — so this is a stale-disk signal, not corruption — but callers on the
    // explicit-save path surface it so the user isn't told a save succeeded when
    // it didn't reach disk.
    // Carry over any top-level project.json keys this writer doesn't manage
    // (e.g. `params`, and fields another tool/the VS Code extension adds like
    // `auto_respawn` / `watchdog_ms`). save_project_locked is a FULL rebuild, so
    // without this it silently DROPS them on every instance CRUD — the same
    // data-loss class as the F1 groups/runtime fix, but for fields a *different*
    // writer owns. Merge-not-clobber: the freshly-built JSON wins for keys it
    // emits; every other top-level key from the existing file survives verbatim.
    static std::string merge_unknown_top_keys_(const std::string& fresh_json,
                                               const std::filesystem::path& existing) {
        std::error_code ec;
        if (!std::filesystem::exists(existing, ec)) return fresh_json;
        std::ifstream f(existing, std::ios::binary);
        std::string old((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (old.empty()) return fresh_json;
        yyjson_doc* od = yyjson_read(old.data(), old.size(), 0);
        yyjson_doc* nd = yyjson_read(fresh_json.data(), fresh_json.size(), 0);
        std::string result = fresh_json;
        if (od && nd) {
            yyjson_val* oroot = yyjson_doc_get_root(od);
            yyjson_val* nroot = yyjson_doc_get_root(nd);
            if (yyjson_is_obj(oroot) && yyjson_is_obj(nroot)) {
                yyjson_mut_doc* md = yyjson_doc_mut_copy(nd, nullptr);
                yyjson_mut_val* mroot = md ? yyjson_mut_doc_get_root(md) : nullptr;
                if (mroot) {
                    // The top-level keys THIS writer manages. Must be an EXPLICIT
                    // allowlist, not "does the key appear in the fresh JSON": some
                    // managed keys (runtime / plugin_dirs / plugins) are emitted
                    // CONDITIONALLY (omitted when empty). Inferring ownership from
                    // presence would treat an intentionally-emptied managed key as
                    // "unknown" and RESURRECT its old on-disk value — clearing would
                    // never persist. Anything NOT in this set is owned by another
                    // writer (params / auto_respawn / watchdog_ms / future) → carry over.
                    static const char* const kOwned[] = {
                        "name", "script", "parallelism", "runtime",
                        "instances", "plugin_dirs", "plugins",
                    };
                    size_t idx, mx; yyjson_val *k, *v;
                    yyjson_obj_foreach(oroot, idx, mx, k, v) {
                        const char* key = yyjson_get_str(k);
                        if (!key) continue;
                        bool owned = false;
                        for (const char* o : kOwned) if (std::strcmp(key, o) == 0) { owned = true; break; }
                        if (owned) continue;                      // managed (even when omitted) → don't resurrect
                        if (yyjson_obj_get(nroot, key)) continue; // also present in fresh → new wins
                        yyjson_mut_val* mk = yyjson_mut_strcpy(md, key);
                        yyjson_mut_val* mv = yyjson_val_mut_copy(md, v);
                        if (mk && mv) yyjson_mut_obj_add(mroot, mk, mv);
                    }
                    if (char* w = yyjson_mut_write(md, YYJSON_WRITE_PRETTY, nullptr)) {
                        result.assign(w);
                        free(w);
                    }
                }
                if (md) yyjson_mut_doc_free(md);
            }
        }
        if (od) yyjson_doc_free(od);
        if (nd) yyjson_doc_free(nd);
        return result;
    }

    bool save_project_locked() {
        auto pj = std::filesystem::path(project_.folder_path) / "project.json";
        std::string out = "{\n";
        out += "  \"name\": "; pm_json_escape(out, project_.name); out += ",\n";
        out += "  \"script\": ";
        pm_json_escape(out, std::filesystem::path(project_.script_path).filename().string());
        out += ",\n";
        out += "  \"parallelism\": {";
        out += "\"dispatch_threads\":" + std::to_string(project_.dispatch_threads);
        out += ",\"queue_depth\":"     + std::to_string(project_.queue_depth);
        out += ",\"overflow\":";
        pm_json_escape(out, project_.overflow);
        out += ",\"result_order\":";
        pm_json_escape(out, project_.result_order);
        // Round-trip dispatch groups + default_group (F1 data-loss: omitting them
        // meant any instance CRUD silently wiped the user's group/priority/affinity
        // config). cpu_affinity is always written nested ([[...]]) — semantically
        // identical to the flat form the parser also accepts.
        if (!project_.groups.empty()) {
            out += ",\"groups\":[";
            for (size_t gi = 0; gi < project_.groups.size(); ++gi) {
                const auto& g = project_.groups[gi];
                if (gi) out += ",";
                out += "{\"name\":"; pm_json_escape(out, g.name);
                out += ",\"max_parallel\":" + std::to_string(g.max_parallel);
                out += ",\"thread_priority\":"; pm_json_escape(out, g.thread_priority);
                out += ",\"queue_depth\":" + std::to_string(g.queue_depth);
                out += ",\"overflow\":"; pm_json_escape(out, g.overflow);
                out += ",\"result_order\":"; pm_json_escape(out, g.result_order);
                out += ",\"min_interval_ms\":" + std::to_string(g.min_interval_ms);
                if (!g.cpu_affinity.empty()) {
                    out += ",\"cpu_affinity\":[";
                    for (size_t ai = 0; ai < g.cpu_affinity.size(); ++ai) {
                        if (ai) out += ",";
                        out += "[";
                        for (size_t ci = 0; ci < g.cpu_affinity[ai].size(); ++ci) {
                            if (ci) out += ",";
                            out += std::to_string(g.cpu_affinity[ai][ci]);
                        }
                        out += "]";
                    }
                    out += "]";
                }
                out += "}";
            }
            out += "]";
        }
        if (!project_.default_group.empty()) {
            out += ",\"default_group\":"; pm_json_escape(out, project_.default_group);
        }
        out += "},\n";
        // runtime block (process_priority + timer_fps) — also round-tripped (F1).
        if (!project_.runtime_priority.empty() || project_.runtime_timer_fps >= 0) {
            out += "  \"runtime\": {";
            bool rfirst = true;
            if (!project_.runtime_priority.empty()) {
                out += "\"process_priority\":"; pm_json_escape(out, project_.runtime_priority);
                rfirst = false;
            }
            if (project_.runtime_timer_fps >= 0) {
                if (!rfirst) out += ",";
                out += "\"timer_fps\":" + std::to_string(project_.runtime_timer_fps);
            }
            out += "},\n";
        }
        out += "  \"instances\": [";
        int i = 0;
        for (auto& [k, v] : project_.instances) {
            if (i++) out += ",";
            out += "\n    {\"name\": "; pm_json_escape(out, v.name);
            out += ", \"plugin\": ";   pm_json_escape(out, v.plugin_name);
            out += "}";
        }
        out += "\n  ]";
        // Round-trip the declarative plugin model (a save must not drop it).
        if (!project_.plugin_dirs.empty()) {
            out += ",\n  \"plugin_dirs\": [";
            for (size_t j = 0; j < project_.plugin_dirs.size(); ++j) {
                if (j) out += ", ";
                pm_json_escape(out, project_.plugin_dirs[j]);
            }
            out += "]";
        }
        if (!project_.plugins.empty()) {
            out += ",\n  \"plugins\": {";
            int j = 0;
            for (auto& p : project_.plugins) {
                if (j++) out += ",";
                out += "\n    "; pm_json_escape(out, p.label);
                out += ": {\"path\": "; pm_json_escape(out, p.path);
                out += ", \"compile\": " + std::string(p.compile ? "true" : "false") + "}";
            }
            out += "\n  }";
        }
        out += "\n}\n";
        // Preserve top-level keys owned by another writer (params / auto_respawn /
        // watchdog_ms / …) instead of clobbering them on every CRUD.
        out = merge_unknown_top_keys_(out, pj);
        // D-P1-5: atomic_write may fail (disk full / read-only / etc.).
        // Bubble that up — silently losing project.json was the audit
        // finding. Caller can't really recover, but at least logs it.
        if (!xi::atomic_write(pj, out)) {
            std::fprintf(stderr,
                "[xinsp2] save_project_locked: atomic_write failed for %s "
                "(disk full / read-only?). Project state on disk may be stale.\n",
                pj.string().c_str());
            return false;
        }
        return true;
    }


    bool save_instance_json(const InstanceInfo& ii) {
        auto path = std::filesystem::path(ii.folder_path) / "instance.json";
        std::string out = "{\n";
        out += "  \"plugin\": "; pm_json_escape(out, ii.plugin_name); out += ",\n";
        // Preserve the per-instance concurrency cap across saves (else a UI save
        // would silently drop a hand-set max_concurrency).
        if (ii.max_concurrency > 0)
            out += "  \"max_concurrency\": " + std::to_string(ii.max_concurrency) + ",\n";
        // Round-trip the dispatch group (F3: a UI save / create / rename used to
        // drop it, so the source's triggers silently fell back to default_group).
        if (!ii.group.empty()) {
            out += "  \"group\": "; pm_json_escape(out, ii.group); out += ",\n";
        }
        out += "  \"config\": ";
        out += ii.instance ? ii.instance->get_def() : "{}";
        out += "\n}\n";
        // D-P1-5: surface atomic_write failures so the user knows their
        // edit didn't reach disk.
        if (!xi::atomic_write(path, out)) {
            std::fprintf(stderr,
                "[xinsp2] save_instance_json: atomic_write failed for %s "
                "(disk full / read-only?). Instance config on disk may be stale.\n",
                path.string().c_str());
            return false;
        }
        return true;
    }
};

} // namespace xi
