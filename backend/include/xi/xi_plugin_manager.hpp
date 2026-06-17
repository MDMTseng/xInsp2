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
#include "xi_baseline.hpp"
#include "xi_cabi_adapter.hpp" // plugin_abi_compatible / PluginInfo / CAbiInstanceAdapter
#include "xi_cert.hpp"
#include "xi_image_pool.hpp"
#include "xi_instance.hpp"
#include "xi_pm_json.hpp"      // pm_json_escape / pm_json_quote (extracted leaf)
#include "xi_pm_parse.hpp"     // parse_manifest / extract_string / validate_config_against_manifest
#include "xi_project_model.hpp" // ProjectInfo / InstanceInfo / CompileEnv / OpenWarning (data model)
#include "xi_resource_store.hpp" // install_resource_hooks (emit/fetch: emit_resource)
#include "xi_safe_state.hpp"   // install_safe_state_hook (set_safe_state)
#include "xi_script_compiler.hpp"
#include "xi_source.hpp"
#include "xi_trigger_bus.hpp"

#include "yyjson.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
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
            auto manifest = entry.path() / "plugin.json";
            if (!std::filesystem::exists(manifest)) continue;
            auto info = parse_manifest(manifest.string(), entry.path().string());
            if (info.name.empty()) continue;
            auto existing = plugins_.find(info.name);
            if (existing != plugins_.end() && existing->second.handle) {
                // Preserve the live handle + factories; update only the
                // fields that can legitimately change between scans.
                existing->second.description   = info.description;
                existing->second.has_ui        = info.has_ui;
                existing->second.reentrant      = info.reentrant;
                existing->second.ui_path       = info.ui_path;
                existing->second.folder_path   = info.folder_path;
                existing->second.manifest_json = info.manifest_json;
            } else {
                plugins_[info.name] = std::move(info);
            }
            count++;
        }
        return count;
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

    // Newest write-time (ticks) over a cmake plugin's source tree — the rebuild
    // change-gate input. Counts source + build-script files, skips the build/
    // tree (its artifacts aren't "source"). 0 if nothing found.
    static uint64_t newest_source_mtime_(const std::string& dir) {
        uint64_t newest = 0;
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec)) return 0;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory()) {
                if (it->path().filename() == "build") it.disable_recursion_pending();
                continue;
            }
            auto ext = it->path().extension().string();
            auto fn  = it->path().filename().string();
            bool src = ext == ".c"  || ext == ".cpp" || ext == ".cc"  || ext == ".cxx" ||
                       ext == ".cu" || ext == ".cuh" || ext == ".h"   || ext == ".hpp" ||
                       ext == ".hxx"|| fn == "CMakeLists.txt" || ext == ".cmake";
            if (!src) continue;
            auto wt = std::filesystem::last_write_time(it->path(), ec);
            if (!ec) newest = std::max(newest, (uint64_t)wt.time_since_epoch().count());
        }
        return newest;
    }

    // Run a command, capturing combined stdout+stderr into *log. Returns the
    // process exit code (or -1 if it couldn't be spawned).
    static int run_cmd_capture_(const std::string& cmd, std::string& log) {
#ifdef _WIN32
        // _popen routes through cmd.exe; the extra outer quotes keep a quoted
        // exe path + quoted args parsing correctly.
        std::string full = "\"" + cmd + " 2>&1\"";
        FILE* pipe = _popen(full.c_str(), "r");
        if (!pipe) { log += "[failed to spawn: " + cmd + "]\n"; return -1; }
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) log += buf;
        int rc = _pclose(pipe);
        return rc;
#else
        // TODO(linux): popen(cmd + " 2>&1") — same shape, no outer-quote wrap.
        (void)cmd; log += "[cmake build unsupported on this platform]\n"; return -1;
#endif
    }

    // Configure (first time) + build one cmake plugin. Appends logs. 0 = success.
    int build_cmake_plugin_(const std::string& cmake_exe, const std::string& src_dir,
                            const std::string& config, const std::string& xinsp_root,
                            std::string& log) {
        auto build_dir = (std::filesystem::path(src_dir) / "build").string();
        auto q = [](const std::string& s) { return "\"" + s + "\""; };
        if (!std::filesystem::exists(std::filesystem::path(build_dir) / "CMakeCache.txt")) {
            std::string cfg = q(cmake_exe) + " -S " + q(src_dir) + " -B " + q(build_dir) +
                              " -A x64 -DXINSP2_ROOT=" + q(xinsp_root);
            // OpenCV: pass the resolved `x64/vcNN/lib` SUBDIR (the level whose
            // OpenCVConfig.cmake actually resolves), NOT the top-level pack dir —
            // forcing the top dir makes OpenCV's runtime auto-detect set
            // OpenCV_FOUND=FALSE. compile_env_.opencv_dir is the host's top pack
            // dir; derive the vc subdir so a NON-standard OpenCV location also
            // works. If none matches, omit it and let xinsp2_plugin.cmake's own
            // (hard-coded C:/opencv) probe try.
            if (!compile_env_.opencv_dir.empty()) {
                for (const char* vc : {"vc17", "vc16", "vc15", "vc14"}) {
                    auto lib = std::filesystem::path(compile_env_.opencv_dir) / "x64" / vc / "lib";
                    if (std::filesystem::exists(lib / "OpenCVConfig.cmake")) {
                        cfg += " -DOpenCV_DIR=" + q(lib.string());
                        break;
                    }
                }
            }
            log += "[configure] " + cfg + "\n";
            int rc = run_cmd_capture_(cfg, log);
            if (rc != 0) return rc;
        }
        std::string bld = q(cmake_exe) + " --build " + q(build_dir) + " --config " + config;
        log += "[build] " + bld + "\n";
        return run_cmd_capture_(bld, log);
    }

    // One instance preserved across a plugin reload: its name + folder +
    // serialized def, captured before destruction and replayed after reload.
    struct PendingInstance {
        std::string name, folder, def_json;
        int         max_concurrency = 0;
    };

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
            pi_it->second.factory = nullptr;
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
        bool has_destroy = GetProcAddress(pi.handle, "xi_plugin_destroy") != nullptr;
        if (has_destroy) {
            pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
        } else {
            pi.factory = reinterpret_cast<PluginInfo::FactoryFn>(
                GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
        }
        if (!pi.c_factory && !pi.factory) {
            if (err) *err = "factory '" + pi.factory_symbol + "' not exported in "
                            "rebuilt DLL — instances for this plugin are gone; "
                            "reopen the project to recover";
            return false;
        }

        xi_host_api& host = default_host_api();
        for (auto& p : pending) {
            std::shared_ptr<InstanceBase> inst;
            if (pi.c_factory) {
                void* raw = pi.c_factory(&host, p.name.c_str());
                if (raw) inst = std::make_shared<CAbiInstanceAdapter>(
                    p.name, plugin_name, pi.handle, raw, pi.reentrant, p.max_concurrency);
            } else if (pi.factory) {
                auto* raw = pi.factory(p.name.c_str());
                if (raw) inst.reset(raw);
            }
            if (!inst) continue;
            if (!p.def_json.empty()) inst->set_def(p.def_json);
            project_.instances[p.name].instance = inst;
            InstanceRegistry::instance().add(inst);
            attach_trigger_bridge(inst.get(), p.name);
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
        auto root = std::filesystem::path(project_folder) / "plugins";
        if (!std::filesystem::exists(root)) return 0;

        int ok_count = 0;
        for (auto& entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_directory()) continue;
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
                    prev->second.factory = nullptr;
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
                bool has_destroy = GetProcAddress(pi.handle, "xi_plugin_destroy") != nullptr;
                if (has_destroy) {
                    pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                        GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
                } else {
                    pi.factory = reinterpret_cast<PluginInfo::FactoryFn>(
                        GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
                }
                if (!pi.c_factory && !pi.factory) {
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
            if (!pi_old.c_factory && !pi_old.factory) return;
            xi_host_api& host = default_host_api();
            for (auto& p : pending) {
                std::shared_ptr<InstanceBase> inst;
                if (pi_old.c_factory) {
                    void* raw = pi_old.c_factory(&host, p.name.c_str());
                    if (raw) inst = std::make_shared<CAbiInstanceAdapter>(
                        p.name, plugin_name, pi_old.handle, raw, pi_old.reentrant, p.max_concurrency);
                } else if (pi_old.factory) {
                    auto* raw = pi_old.factory(p.name.c_str());
                    if (raw) inst.reset(raw);
                }
                if (!inst) continue;
                if (!p.def_json.empty()) inst->set_def(p.def_json);
                project_.instances[p.name].instance = inst;
                InstanceRegistry::instance().add(inst);
                attach_trigger_bridge(inst.get(), p.name);
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
            //
            // P0-D4: previously only handled the c_factory branch; an
            // old-ABI plugin (using the legacy `factory` C++ symbol)
            // had its instances destroyed in step 1 but never
            // re-attached. The instances dict entries were left with
            // null `instance`; subsequent calls silently no-op.
            // Handle BOTH old- and new-ABI factory shapes here.
            auto pi_it = plugins_.find(plugin_name);
            if (pi_it != plugins_.end()
                && (pi_it->second.c_factory || pi_it->second.factory)) {
                xi_host_api& host = default_host_api();
                for (auto& p : pending) {
                    std::shared_ptr<InstanceBase> inst;
                    if (pi_it->second.c_factory) {
                        void* raw = pi_it->second.c_factory(&host, p.name.c_str());
                        if (raw) inst = std::make_shared<CAbiInstanceAdapter>(
                            p.name, plugin_name, pi_it->second.handle, raw, pi_it->second.reentrant, p.max_concurrency);
                    } else if (pi_it->second.factory) {
                        auto* raw = pi_it->second.factory(p.name.c_str());
                        if (raw) inst.reset(raw);
                    }
                    if (!inst) continue;
                    if (!p.def_json.empty()) inst->set_def(p.def_json);
                    project_.instances[p.name].instance = inst;
                    InstanceRegistry::instance().add(inst);
                    attach_trigger_bridge(inst.get(), p.name);
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
            pi.factory   = nullptr;
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
        bool has_destroy = GetProcAddress(pi.handle, "xi_plugin_destroy") != nullptr;
        if (has_destroy) {
            pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
        } else {
            pi.factory = reinterpret_cast<PluginInfo::FactoryFn>(
                GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
        }
        if (!pi.c_factory && !pi.factory) {
            r.error = "factory '" + pi.factory_symbol + "' not exported in new DLL"
                      " — instances for this plugin are gone; reopen the "
                      "project to recover";
            return r;
        }

        // Refresh the change-gate stamp so a later reload_changed_plugins()
        // doesn't see this freshly-recompiled DLL as "changed" and swap it again.
        stamp_loaded_dll_(pi, cres.dll_path);

        // 4. Re-instantiate every preserved instance using the new factory.
        xi_host_api& host = default_host_api();
        for (auto& p : pending) {
            std::shared_ptr<InstanceBase> inst;
            if (pi.c_factory) {
                void* raw = pi.c_factory(&host, p.name.c_str());
                if (raw) inst = std::make_shared<CAbiInstanceAdapter>(
                    p.name, plugin_name, pi.handle, raw, pi.reentrant, p.max_concurrency);
            } else if (pi.factory) {
                auto* raw = pi.factory(p.name.c_str());
                if (raw) inst.reset(raw);
            }
            if (!inst) continue;
            if (!p.def_json.empty()) inst->set_def(p.def_json);
            project_.instances[p.name].instance = inst;
            InstanceRegistry::instance().add(inst);
            attach_trigger_bridge(inst.get(), p.name);
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
            if (loaded && newest_source_mtime_(src_dir) <= it->second.loaded_dll_mtime) {
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
                        j.rc = build_cmake_plugin_(cmake_exe, j.src_dir, config, xinsp_root, j.log);
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

    // Export a project plugin as a standalone deployable folder. Steps:
    //   1. Recompile in PluginExport mode (/O2 /Zi — Release with PDB).
    //   2. LoadLibrary the new DLL into a temporary handle.
    //   3. Run baseline tests (cert::certify) — if they fail, the export
    //      aborts so we never ship an uncertified plugin.
    //   4. Copy <plugin>.dll, <plugin>.pdb, cert.json, plugin.json (auto-
    //      generated if missing), and any ui/ subfolder into <dest>/<name>/.
    //   5. Free the temporary handle (the in-process plugin keeps its
    //      Dev DLL — export doesn't disturb the running session).
    struct ExportResult {
        bool                     ok = false;
        std::string              dest_dir;       // <dest>/<name>
        std::string              error;
        std::string              build_log;
        std::vector<xi::script::Diagnostic> diagnostics;
        bool                     cert_passed = false;
        int                      cert_pass_count = 0;
        int                      cert_fail_count = 0;
    };
    ExportResult export_project_plugin(const std::string& plugin_name,
                                        const std::string& dest_root) {
        std::lock_guard<std::mutex> lk(mu_);
        ExportResult er;
        auto orig_it = project_plugin_origin_.find(plugin_name);
        if (orig_it == project_plugin_origin_.end()) {
            er.error = "not a project plugin: " + plugin_name;
            return er;
        }
        auto src_dir = std::filesystem::path(orig_it->second);
        auto pi_it = plugins_.find(plugin_name);
        if (pi_it == plugins_.end()) {
            er.error = "plugin entry missing: " + plugin_name;
            return er;
        }
        auto& pi = pi_it->second;

        // Re-collect sources (mirror of compile_project_plugins_locked).
        std::vector<std::string> sources;
        auto walk = [&](const std::filesystem::path& dir) { collect_cpp_sources(dir, sources); };
        auto src_subdir = src_dir / "src";
        if (std::filesystem::exists(src_subdir)) walk(src_subdir);
        else                                     walk(src_dir);
        if (sources.empty()) { er.error = "no .cpp sources"; return er; }
        std::vector<std::string> includes;
        auto inc_dir = src_dir / "include";
        if (std::filesystem::exists(inc_dir)) includes.push_back(inc_dir.string());

        // Build into a separate export/ folder so the dev DLL isn't touched.
        auto export_build = src_dir / "export_build";
        xi::script::CompileRequest req;
        req.source_path    = sources.front();
        req.extra_sources.assign(sources.begin() + 1, sources.end());
        req.include_dirs   = includes;
        req.output_dir     = export_build.string();
        req.include_dir    = compile_env_.include_dir;
        req.vcvars_path    = compile_env_.vcvars_path;
        req.opencv_dir     = compile_env_.opencv_dir;
        req.turbojpeg_root = compile_env_.turbojpeg_root;
        req.ipp_root       = compile_env_.ipp_root;
        req.mode           = xi::script::CompileMode::PluginExport;

        std::fprintf(stderr, "[xinsp2] export: compiling '%s' (Release)...\n",
                     plugin_name.c_str());
        auto cres = xi::script::compile(req);
        er.build_log   = cres.build_log;
        er.diagnostics = cres.diagnostics;
        if (!cres.ok) { er.error = "Release compile failed"; return er; }

        // Load the freshly built DLL into a temp handle for cert.
        HMODULE temp = LoadLibraryA(cres.dll_path.c_str());
        if (!temp) { er.error = "LoadLibrary failed on Release DLL"; return er; }

        // Run baseline. cert::certify writes cert.json beside the DLL on pass.
        auto syms = xi::baseline::load_symbols(temp);
        static xi_host_api cert_host = ImagePool::make_host_api();
        auto build_dir = std::filesystem::path(cres.dll_path).parent_path();
        std::fprintf(stderr, "[xinsp2] export: running baseline...\n");
        auto summary = xi::cert::certify(build_dir, cres.dll_path,
                                          plugin_name, syms, &cert_host);
        FreeLibrary(temp);
        er.cert_pass_count = summary.pass_count;
        er.cert_fail_count = summary.fail_count;
        er.cert_passed     = summary.all_passed;
        if (!summary.all_passed) {
            er.error = "baseline certification failed: "
                     + std::to_string(summary.pass_count) + "/"
                     + std::to_string(summary.pass_count + summary.fail_count)
                     + " passed";
            return er;
        }

        // Copy the deployable into dest_root/<plugin_name>/.
        auto dest = std::filesystem::path(dest_root) / plugin_name;
        std::error_code ec;
        std::filesystem::create_directories(dest, ec);

        // DLL — rename _v<n> versioning out so the deployed file is stable.
        auto dll_dest = dest / (plugin_name + ".dll");
        std::filesystem::copy_file(cres.dll_path, dll_dest,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { er.error = "copy DLL: " + ec.message(); return er; }

        // PDB beside DLL — keep so end-user crashes can blame source line.
        auto pdb_src = std::filesystem::path(cres.dll_path)
                           .replace_extension(".pdb");
        if (std::filesystem::exists(pdb_src)) {
            std::filesystem::copy_file(pdb_src,
                dest / (plugin_name + ".pdb"),
                std::filesystem::copy_options::overwrite_existing, ec);
        }

        // cert.json
        auto cert_src = build_dir / "cert.json";
        if (std::filesystem::exists(cert_src)) {
            std::filesystem::copy_file(cert_src,
                dest / "cert.json",
                std::filesystem::copy_options::overwrite_existing, ec);
        }

        // plugin.json — synthesize from PluginInfo if there's no manifest in
        // the source folder. Generated form points dll/factory at the names
        // the export uses, so the deployed folder is self-contained. The
        // synthesized version stamps `abi_version` so a target backend
        // older than the plugin's compile-time ABI can detect the
        // mismatch on scan (matches the runtime plugin_abi_compatible
        // check via the DLL's xi_plugin_abi_version export).
        auto src_manifest = src_dir / "plugin.json";
        std::string manifest_text;
        if (std::filesystem::exists(src_manifest)) {
            std::ifstream mf(src_manifest.string());
            std::stringstream ms; ms << mf.rdbuf();
            manifest_text = ms.str();
        } else {
            manifest_text = "{\n";
            manifest_text += "  \"name\":        \"" + pi.name + "\",\n";
            manifest_text += "  \"description\": \"" + pi.description + "\",\n";
            manifest_text += "  \"dll\":         \"" + pi.name + ".dll\",\n";
            manifest_text += "  \"factory\":     \"" + pi.factory_symbol + "\",\n";
            manifest_text += "  \"has_ui\":      " + std::string(pi.has_ui ? "true" : "false") + ",\n";
            manifest_text += "  \"abi_version\": " + std::to_string(XI_ABI_VERSION) + "\n";
            manifest_text += "}\n";
        }
        xi::atomic_write(dest / "plugin.json", manifest_text);

        // ui/ — copy whole subtree if present.
        auto ui_src = src_dir / "ui";
        if (std::filesystem::exists(ui_src)) {
            std::filesystem::copy(ui_src, dest / "ui",
                std::filesystem::copy_options::recursive |
                std::filesystem::copy_options::overwrite_existing, ec);
        }

        er.ok       = true;
        er.dest_dir = dest.string();
        std::fprintf(stderr, "[xinsp2] exported '%s' to %s\n",
                     plugin_name.c_str(), er.dest_dir.c_str());
        return er;
    }

    // Was this plugin loaded from inside the current project (vs. global)?
    bool is_project_plugin(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        return project_plugin_origin_.count(name) > 0;
    }

    // Load a plugin's DLL and resolve the factory function.
    //
    // Side effect (new ABI only): if the plugin has no cert.json, or the
    // cert is out of date relative to the DLL or the current
    // BASELINE_VERSION, runs baseline tests and writes cert.json on pass.
    // A failed certification unloads the DLL and returns false — the
    // plugin cannot be instantiated until the developer fixes the issue.
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

        // Distinguish new vs old ABI: new ABI also exports xi_plugin_destroy
        auto has_destroy = GetProcAddress(pi.handle, "xi_plugin_destroy") != nullptr;
        if (has_destroy) {
            // New C ABI: factory takes (xi_host_api*, const char*) → void*
            pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                GetProcAddress(pi.handle, pi.factory_symbol.c_str()));

            // Run certification (baseline tests) if cert is missing/stale.
            auto plugin_folder = std::filesystem::path(pi.folder_path);
            if (!xi::cert::is_valid(plugin_folder, dll_path)) {
                std::fprintf(stderr, "[xinsp2] certifying plugin '%s'...\n", name.c_str());
                auto syms = xi::baseline::load_symbols(pi.handle);
                static xi_host_api cert_host = ImagePool::make_host_api();
                auto summary = xi::cert::certify(plugin_folder, dll_path, name, syms, &cert_host);
                if (summary.all_passed) {
                    std::fprintf(stderr, "[xinsp2] cert OK '%s' (%d tests, %.0fms)\n",
                                 name.c_str(), summary.pass_count, summary.total_ms);
                } else {
                    std::fprintf(stderr, "[xinsp2] cert FAILED '%s' — %d/%d passed:\n",
                                 name.c_str(), summary.pass_count,
                                 summary.pass_count + summary.fail_count);
                    for (auto& r : summary.results) {
                        if (!r.passed) {
                            std::fprintf(stderr, "  - %s: %s\n", r.name.c_str(), r.error.c_str());
                        }
                    }
                    FreeLibrary(pi.handle);
                    pi.handle = nullptr;
                    pi.c_factory = nullptr;
                    return fail("plugin '" + name + "': certification failed (" +
                                std::to_string(summary.fail_count) + " baseline test(s) failed; "
                                "see backend log for details)");
                }
            }
        } else {
            // Old-style: factory takes (const char*) → InstanceBase*
            pi.factory = reinterpret_cast<PluginInfo::FactoryFn>(
                GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
        }
        if (pi.c_factory == nullptr && pi.factory == nullptr)
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
    static constexpr const char* kWorkingCopyDir = ".xinsp_work";
    // Commit-in-progress journal marker. Written at the canonical root before a
    // commit's mirror starts and removed after it completes. If it survives (a
    // crash/power-loss mid-commit left the canonical tree torn), the next
    // open_project rolls the commit forward from the intact scratch.
    static constexpr const char* kCommitMarker = ".xinsp_commit_pending";

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
        { std::ofstream mf(marker); mf << "commit in progress\n"; }
        mirror_tree_(project_.folder_path, canonical_path_);
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
                mirror_tree_(scratch, canon);
                std::filesystem::remove(marker, ec);
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
                copy_tree_excluding_(canon, scratch);
                std::fprintf(stderr, "[xinsp2] working copy: seeded %s from project\n",
                             scratch.string().c_str());
            } else {
                std::fprintf(stderr, "[xinsp2] working copy: resuming existing %s\n",
                             scratch.string().c_str());
            }
            ensure_gitignore_(canon, std::string(kWorkingCopyDir) + "/");
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
        project_.trigger_policy    = TriggerPolicy::Any;
        project_.trigger_required.clear();
        project_.trigger_leader.clear();
        project_.trigger_window_ms = 100;
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
        yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (root) {
            if (yyjson_val* tp = yyjson_obj_get(root, "trigger_policy");
                tp && yyjson_is_obj(tp)) {
                if (yyjson_val* k = yyjson_obj_get(tp, "policy");
                    k && yyjson_is_str(k) && yyjson_get_str(k)) {
                    std::string p = yyjson_get_str(k);
                    if      (p == "all_required")     project_.trigger_policy = TriggerPolicy::AllRequired;
                    else if (p == "leader_followers") project_.trigger_policy = TriggerPolicy::LeaderFollowers;
                }
                if (yyjson_val* k = yyjson_obj_get(tp, "leader");
                    k && yyjson_is_str(k) && yyjson_get_str(k)) {
                    project_.trigger_leader = yyjson_get_str(k);
                }
                if (yyjson_val* k = yyjson_obj_get(tp, "window_ms");
                    k && yyjson_is_num(k)) {
                    project_.trigger_window_ms = (int)yyjson_get_num(k);
                }
                if (yyjson_val* arr = yyjson_obj_get(tp, "required");
                    arr && yyjson_is_arr(arr)) {
                    size_t _i, _n; yyjson_val* it;
                    yyjson_arr_foreach(arr, _i, _n, it) {
                        if (yyjson_is_str(it) && yyjson_get_str(it)) {
                            project_.trigger_required.emplace_back(yyjson_get_str(it));
                        }
                    }
                }
            }
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
        TriggerBus::instance().set_policy(
            project_.trigger_policy, project_.trigger_required,
            project_.trigger_leader, project_.trigger_window_ms);

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
        int proj_plugins = compile_project_plugins_locked(folder);
        if (proj_plugins > 0) {
            std::fprintf(stderr, "[xinsp2] %d project plugin(s) built\n", proj_plugins);
        }

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
                if (pit != plugins_.end() && !pit->second.factory && !pit->second.c_factory) {
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
                            auto has_destroy = GetProcAddress(pi2.handle, "xi_plugin_destroy") != nullptr;
                            if (has_destroy)
                                pi2.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                                    GetProcAddress(pi2.handle, pi2.factory_symbol.c_str()));
                            else
                                pi2.factory = reinterpret_cast<PluginInfo::FactoryFn>(
                                    GetProcAddress(pi2.handle, pi2.factory_symbol.c_str()));
                            // P0-D3 (cont.): if neither factory symbol
                            // resolves, the DLL is loaded but unusable.
                            // Previously left in place with handle set
                            // and factory=null; subsequent open_project
                            // attempts found a stale entry. FreeLibrary
                            // and clear handle so the entry stays in a
                            // clean "not loaded" state.
                            if (!pi2.factory && !pi2.c_factory) {
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
                        // Pre-allocate the owner id and install a guard
                        // around the ctor itself so any host->image_create
                        // calls inside xi_plugin_create are tagged. If
                        // the ctor returns null OR throws, sweep — the
                        // adapter never got built, so its destructor
                        // can't sweep for us.
                        ImagePoolOwnerId pre_owner = ImagePool::alloc_owner_id();
                        void* raw = nullptr;
                        try {
                            ImagePool::OwnerGuard og(pre_owner);
                            raw = pi.c_factory(&host, ii.name.c_str());
                        } catch (...) {
                            ImagePool::instance().release_all_for(pre_owner);
                            throw;
                        }
                        if (raw) {
                            auto adapter = std::make_shared<CAbiInstanceAdapter>(
                                ii.name, *plugin, pi.handle, raw, pi.reentrant, ii.max_concurrency);
                            // Hand the pre-allocated owner id over to the
                            // adapter so subsequent process / exchange
                            // calls keep tagging into the same bucket.
                            adapter->adopt_owner_id(pre_owner);
                            ii.instance = std::move(adapter);
                            created = true;
                        } else {
                            ImagePool::instance().release_all_for(pre_owner);
                        }
                    } else if (!created && pi.factory) {
                        auto* raw = pi.factory(ii.name.c_str());
                        if (raw) {
                            ii.instance.reset(raw);
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
                        attach_trigger_bridge(ii.instance.get(), ii.name);
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

    // Create a new instance of a plugin inside the current project.
    InstanceInfo* create_instance(const std::string& instance_name,
                                   const std::string& plugin_name,
                                   std::string* err = nullptr) {
        auto fail = [&](std::string msg) -> InstanceInfo* { if (err) *err = std::move(msg); return nullptr; };
        std::lock_guard<std::mutex> lk(mu_);
        if (project_.folder_path.empty())
            return fail("no project open — open_project before creating an instance");

        auto pit = plugins_.find(plugin_name);
        if (pit == plugins_.end())
            return fail("plugin '" + plugin_name + "' not loaded");
        auto& pi = pit->second;
        if (!pi.factory && !pi.c_factory)
            return fail("plugin '" + plugin_name + "' has no factory (load_plugin failed?)");

        auto inst_folder = std::filesystem::path(project_.folder_path) / "instances" / instance_name;
        std::filesystem::create_directories(inst_folder);

        InstanceInfo ii;
        ii.name = instance_name;
        ii.plugin_name = plugin_name;
        ii.folder_path = inst_folder.string();

        // Register the folder BEFORE the factory runs so the plugin can
        // call host->instance_folder() from inside its constructor.
        InstanceFolderRegistry::instance().set(instance_name, ii.folder_path);

        if (pi.c_factory) {
            // New C ABI — create via host API. Pre-allocate the owner
            // id so any host->image_create called from inside the
            // plugin's ctor is tagged. Sweep on null return / throw
            // so a half-initialised plugin doesn't leak handles.
            xi_host_api& host = default_host_api();
            ImagePoolOwnerId pre_owner = ImagePool::alloc_owner_id();
            void* raw = nullptr;
            try {
                ImagePool::OwnerGuard og(pre_owner);
                raw = pi.c_factory(&host, instance_name.c_str());
            } catch (...) {
                ImagePool::instance().release_all_for(pre_owner);
                InstanceFolderRegistry::instance().clear(instance_name);
                throw;
            }
            if (!raw) {
                ImagePool::instance().release_all_for(pre_owner);
                InstanceFolderRegistry::instance().clear(instance_name);
                return fail("plugin '" + plugin_name + "' factory returned null "
                            "(constructor failed/rejected the config)");
            }
            auto adapter = std::make_shared<CAbiInstanceAdapter>(
                instance_name, plugin_name, pi.handle, raw, pi.reentrant, /*max_concurrency=*/0);
            adapter->adopt_owner_id(pre_owner);
            ii.instance = std::move(adapter);
        } else {
            // Old-style factory
            auto* raw = pi.factory(instance_name.c_str());
            if (!raw) return fail("plugin '" + plugin_name + "' factory returned null");
            ii.instance.reset(raw);
        }
        InstanceRegistry::instance().add(ii.instance);
        attach_trigger_bridge(ii.instance.get(), instance_name);

        // Save instance.json
        save_instance_json(ii);

        project_.instances[instance_name] = std::move(ii);
        save_project_locked();
        return &project_.instances[instance_name];
    }

    // Bridge legacy xi::ImageSource into the global TriggerBus so trigger-
    // aware scripts see push()'d frames as bus events without each plugin
    // having to migrate. Implemented out-of-line in xi_trigger_bridge.hpp.
    static void attach_trigger_bridge(InstanceBase* inst, const std::string& source);

    // Save an instance's current config to its folder.
    bool save_instance(const std::string& instance_name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = project_.instances.find(instance_name);
        if (it == project_.instances.end()) return false;
        save_instance_json(it->second);
        return true;
    }

    // Remove an instance: destroys the runtime object + unregisters from
    // both registries. Optionally deletes the on-disk folder.
    bool remove_instance(const std::string& instance_name, bool delete_folder) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = project_.instances.find(instance_name);
        if (it == project_.instances.end()) return false;
        InstanceRegistry::instance().remove(instance_name);
        InstanceFolderRegistry::instance().clear(instance_name);
        ImageSource::unregister_publish_hook(instance_name);
        std::string folder = it->second.folder_path;
        project_.instances.erase(it);
        if (delete_folder && !folder.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(folder, ec);
        }
        save_project_locked();
        return true;
    }

    // Rename an instance. Moves the on-disk folder and re-registers under
    // the new name. Returns false if the new name is in use or taken by
    // an on-disk folder not tied to any instance.
    bool rename_instance(const std::string& old_name, const std::string& new_name) {
        std::lock_guard<std::mutex> lk(mu_);
        if (old_name == new_name) return true;
        auto it = project_.instances.find(old_name);
        if (it == project_.instances.end()) return false;
        if (project_.instances.count(new_name)) return false;
        // Move the folder
        auto old_folder = std::filesystem::path(it->second.folder_path);
        auto new_folder = old_folder.parent_path() / new_name;
        if (std::filesystem::exists(new_folder)) return false;
        std::error_code ec;
        std::filesystem::rename(old_folder, new_folder, ec);
        if (ec) return false;
        // Update registries — InstanceBase::name() is immutable, so we
        // recreate the instance under the new name via the plugin factory.
        // Old instance's state is preserved via get_def → set_def.
        auto pit = plugins_.find(it->second.plugin_name);
        if (pit == plugins_.end()) return false;
        auto& pi = pit->second;

        std::string saved_def;
        if (it->second.instance) saved_def = it->second.instance->get_def();

        // Drop old runtime entries before creating new one (same name-map
        // only has one slot, different keys).
        InstanceRegistry::instance().remove(old_name);
        InstanceFolderRegistry::instance().clear(old_name);

        InstanceInfo ii;
        ii.name = new_name;
        ii.plugin_name = it->second.plugin_name;
        ii.folder_path = new_folder.string();
        InstanceFolderRegistry::instance().set(new_name, ii.folder_path);
        if (pi.c_factory) {
            xi_host_api& host = default_host_api();
            void* raw = pi.c_factory(&host, new_name.c_str());
            if (!raw) { InstanceFolderRegistry::instance().clear(new_name); return false; }
            ii.instance = std::make_shared<CAbiInstanceAdapter>(
                new_name, ii.plugin_name, pi.handle, raw, pi.reentrant, ii.max_concurrency);
        } else if (pi.factory) {
            auto* raw = pi.factory(new_name.c_str());
            if (!raw) return false;
            ii.instance.reset(raw);
        } else {
            return false;
        }
        if (!saved_def.empty()) ii.instance->set_def(saved_def);
        InstanceRegistry::instance().add(ii.instance);

        project_.instances.erase(it);
        project_.instances[new_name] = std::move(ii);
        save_instance_json(project_.instances[new_name]);
        save_project_locked();
        return true;
    }

    ProjectInfo& project() { return project_; }

    // Update the trigger policy for the current project. Applies to the
    // global TriggerBus immediately and re-saves project.json.
    bool set_trigger_policy(TriggerPolicy p,
                            std::vector<std::string> required,
                            std::string leader,
                            int window_ms)
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (project_.folder_path.empty()) return false;
        project_.trigger_policy    = p;
        project_.trigger_required  = std::move(required);
        project_.trigger_leader    = std::move(leader);
        project_.trigger_window_ms = window_ms;
        TriggerBus::instance().set_policy(
            p, project_.trigger_required, project_.trigger_leader, window_ms);
        save_project_locked();
        return true;
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
    std::mutex mu_;
    std::unordered_map<std::string, PluginInfo> plugins_;
    ProjectInfo project_;
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
        static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); install_resource_hooks(a); install_safe_state_hook(a); return a; }();
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

    // True if a path component should be skipped when seeding/mirroring the
    // working copy: the scratch dir itself, VCS metadata, and regenerated build
    // output (recompiled on open, no point copying — and committing it back
    // would clobber the canonical build with the scratch's).
    static bool wc_excluded_(const std::filesystem::path& rel) {
        for (const auto& part : rel) {
            std::string s = part.string();
            if (s == kWorkingCopyDir || s == ".git" || s == "build" ||
                s == kCommitMarker) return true;
        }
        return false;
    }

    // Recursively copy `src` -> `dst`, skipping wc_excluded_ paths. Used to seed
    // a fresh working copy from the canonical project.
    static void copy_tree_excluding_(const std::filesystem::path& src,
                                     const std::filesystem::path& dst) {
        std::error_code ec;
        std::filesystem::create_directories(dst, ec);
        for (auto it = std::filesystem::recursive_directory_iterator(
                 src, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            auto rel = std::filesystem::relative(it->path(), src, ec);
            if (ec || rel.empty()) continue;
            if (wc_excluded_(rel)) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
            auto target = dst / rel;
            if (it->is_directory()) {
                std::filesystem::create_directories(target, ec);
            } else {
                std::filesystem::create_directories(target.parent_path(), ec);
                std::filesystem::copy_file(it->path(), target,
                    std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
    }

    // Mirror `src` (working copy) onto `dst` (canonical): copy/overwrite every
    // file, then delete files/dirs in `dst` that aren't in `src` — so removals
    // (e.g. a deleted instance) propagate. Excluded paths are left untouched on
    // both sides (the canonical .git stays; build/ is regenerated).
    static void mirror_tree_(const std::filesystem::path& src,
                             const std::filesystem::path& dst) {
        std::error_code ec;
        copy_tree_excluding_(src, dst);   // adds + overwrites
        // Prune: remove dst entries with no src counterpart.
        std::vector<std::filesystem::path> to_remove;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 dst, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            auto rel = std::filesystem::relative(it->path(), dst, ec);
            if (ec || rel.empty()) continue;
            if (wc_excluded_(rel)) { if (it->is_directory()) it.disable_recursion_pending(); continue; }
            if (!std::filesystem::exists(src / rel)) to_remove.push_back(it->path());
        }
        for (auto& p : to_remove) std::filesystem::remove_all(p, ec);
    }

    // Append `line` to <dir>/.gitignore if not already present (so the scratch
    // dir isn't accidentally committed). Best-effort; ignores I/O errors.
    static void ensure_gitignore_(const std::filesystem::path& dir,
                                  const std::string& line) {
        auto gi = dir / ".gitignore";
        std::string content;
        { std::ifstream f(gi); std::stringstream ss; ss << f.rdbuf(); content = ss.str(); }
        if (content.find(line) != std::string::npos) return;
        std::ofstream f(gi, std::ios::app);
        if (!f) return;
        if (!content.empty() && content.back() != '\n') f << "\n";
        f << line << "\n";
    }

    // json_flag_true / extract_string / detail_find_key / parse_manifest /
    // validate_config_against_manifest moved to xi_pm_parse.hpp (leaf;
    // included above). Called unqualified -> resolve in namespace xi.

    void save_project_locked() {
        auto pj = std::filesystem::path(project_.folder_path) / "project.json";
        std::string out = "{\n";
        out += "  \"name\": "; pm_json_escape(out, project_.name); out += ",\n";
        out += "  \"script\": ";
        pm_json_escape(out, std::filesystem::path(project_.script_path).filename().string());
        out += ",\n";
        out += "  \"trigger_policy\": " + trigger_policy_json_locked() + ",\n";
        out += "  \"parallelism\": {";
        out += "\"dispatch_threads\":" + std::to_string(project_.dispatch_threads);
        out += ",\"queue_depth\":"     + std::to_string(project_.queue_depth);
        out += ",\"overflow\":";
        pm_json_escape(out, project_.overflow);
        out += ",\"result_order\":";
        pm_json_escape(out, project_.result_order);
        out += "},\n";
        out += "  \"instances\": [";
        int i = 0;
        for (auto& [k, v] : project_.instances) {
            if (i++) out += ",";
            out += "\n    {\"name\": "; pm_json_escape(out, v.name);
            out += ", \"plugin\": ";   pm_json_escape(out, v.plugin_name);
            out += "}";
        }
        out += "\n  ]\n}\n";
        // D-P1-5: atomic_write may fail (disk full / read-only / etc.).
        // Bubble that up — silently losing project.json was the audit
        // finding. Caller can't really recover, but at least logs it.
        if (!xi::atomic_write(pj, out)) {
            std::fprintf(stderr,
                "[xinsp2] save_project_locked: atomic_write failed for %s "
                "(disk full / read-only?). Project state on disk may be stale.\n",
                pj.string().c_str());
        }
    }

    std::string trigger_policy_json_locked() const {
        const char* p =
            project_.trigger_policy == TriggerPolicy::AllRequired     ? "all_required" :
            project_.trigger_policy == TriggerPolicy::LeaderFollowers ? "leader_followers" :
                                                                         "any";
        std::string s = "{\"policy\":\"";
        s += p; s += "\",\"window_ms\":";
        s += std::to_string(project_.trigger_window_ms);
        s += ",\"required\":[";
        for (size_t i = 0; i < project_.trigger_required.size(); ++i) {
            if (i) s += ",";
            pm_json_escape(s, project_.trigger_required[i]);
        }
        s += "],\"leader\":";
        pm_json_escape(s, project_.trigger_leader);
        s += "}";
        return s;
    }

    void save_instance_json(const InstanceInfo& ii) {
        auto path = std::filesystem::path(ii.folder_path) / "instance.json";
        std::string out = "{\n";
        out += "  \"plugin\": "; pm_json_escape(out, ii.plugin_name); out += ",\n";
        // Preserve the per-instance concurrency cap across saves (else a UI save
        // would silently drop a hand-set max_concurrency).
        if (ii.max_concurrency > 0)
            out += "  \"max_concurrency\": " + std::to_string(ii.max_concurrency) + ",\n";
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
        }
    }
};

} // namespace xi
