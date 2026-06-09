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

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <sstream>
#include <string>
#include <unordered_map>
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
    int compile_project_plugins_locked(const std::string& project_folder) {
        auto root = std::filesystem::path(project_folder) / "plugins";
        if (!std::filesystem::exists(root)) return 0;

        int ok_count = 0;
        for (auto& entry : std::filesystem::directory_iterator(root)) {
            if (!entry.is_directory()) continue;
            std::string pname = entry.path().filename().string();
            try {
                // Collect .cpp sources: prefer src/ if present, else root.
                std::vector<std::string> sources;
                auto src_dir = entry.path() / "src";
                auto walk = [&](const std::filesystem::path& dir) { collect_cpp_sources(dir, sources); };
                if (std::filesystem::exists(src_dir)) walk(src_dir);
                else                                  walk(entry.path());
                if (sources.empty()) {
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
                req.source_path    = sources.front();
                req.extra_sources.assign(sources.begin() + 1, sources.end());
                req.include_dirs   = includes;
                req.output_dir     = (entry.path() / "build").string();
                req.include_dir    = compile_env_.include_dir;
                req.vcvars_path    = compile_env_.vcvars_path;
                req.opencv_dir     = compile_env_.opencv_dir;
                req.turbojpeg_root = compile_env_.turbojpeg_root;
                req.ipp_root       = compile_env_.ipp_root;
                req.mode           = xi::script::CompileMode::PluginDev;

                xi::script::CompileResult res;
                if (compile_env_.aot) {
                    // AOT bundle: load the newest prebuilt DLL in build/ — no cl.exe.
                    std::filesystem::path build_dir = entry.path() / "build", newest;
                    std::filesystem::file_time_type best{};
                    if (std::filesystem::exists(build_dir))
                        for (auto& f : std::filesystem::directory_iterator(build_dir))
                            if (f.is_regular_file() && f.path().extension() == ".dll") {
                                auto t = std::filesystem::last_write_time(f);
                                if (newest.empty() || t > best) { best = t; newest = f.path(); }
                            }
                    if (newest.empty()) {
                        last_open_warnings_.push_back({pname, pname, "AOT: no prebuilt DLL in build/ (export first)"});
                        std::fprintf(stderr, "[xinsp2] AOT: plugin '%s' has no prebuilt DLL — skipped\n", pname.c_str());
                        continue;
                    }
                    res.ok = true; res.dll_path = newest.string();
                    std::fprintf(stderr, "[xinsp2] AOT: loading prebuilt plugin '%s': %s\n", pname.c_str(), res.dll_path.c_str());
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
                    if (!plugin_abi_compatible(pi.handle, pname, &err)) {
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
            if (!plugin_abi_compatible(pi.handle, plugin_name, &err)) {
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
            if (!plugin_abi_compatible(pi.handle, name, &aerr)) {
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
        // have none and we default to Any). Use cJSON instead of
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
        if (cJSON* root = cJSON_Parse(content.c_str())) {
            if (cJSON* tp = cJSON_GetObjectItem(root, "trigger_policy");
                tp && cJSON_IsObject(tp)) {
                if (cJSON* k = cJSON_GetObjectItem(tp, "policy");
                    k && cJSON_IsString(k) && k->valuestring) {
                    std::string p = k->valuestring;
                    if      (p == "all_required")     project_.trigger_policy = TriggerPolicy::AllRequired;
                    else if (p == "leader_followers") project_.trigger_policy = TriggerPolicy::LeaderFollowers;
                }
                if (cJSON* k = cJSON_GetObjectItem(tp, "leader");
                    k && cJSON_IsString(k) && k->valuestring) {
                    project_.trigger_leader = k->valuestring;
                }
                if (cJSON* k = cJSON_GetObjectItem(tp, "window_ms");
                    k && cJSON_IsNumber(k)) {
                    project_.trigger_window_ms = (int)k->valuedouble;
                }
                if (cJSON* arr = cJSON_GetObjectItem(tp, "required");
                    arr && cJSON_IsArray(arr)) {
                    cJSON* it;
                    cJSON_ArrayForEach(it, arr) {
                        if (cJSON_IsString(it) && it->valuestring) {
                            project_.trigger_required.emplace_back(it->valuestring);
                        }
                    }
                }
            }
            // runtime block — operational knobs (also live-settable). process_priority
            // applied on open; timer_fps seeds the live timer rate.
            if (cJSON* rt = cJSON_GetObjectItem(root, "runtime"); rt && cJSON_IsObject(rt)) {
                if (cJSON* k = cJSON_GetObjectItem(rt, "process_priority"); k && cJSON_IsString(k) && k->valuestring)
                    project_.runtime_priority = k->valuestring;
                if (cJSON* k = cJSON_GetObjectItem(rt, "timer_fps"); k && cJSON_IsNumber(k))
                    project_.runtime_timer_fps = (int)k->valuedouble;
            }
            // parallelism block.
            if (cJSON* par = cJSON_GetObjectItem(root, "parallelism");
                par && cJSON_IsObject(par)) {
                if (cJSON* k = cJSON_GetObjectItem(par, "dispatch_threads");
                    k && cJSON_IsNumber(k)) {
                    int n = (int)k->valuedouble;
                    if (n < 1) n = 1;
                    if (n > 32) n = 32;  // sanity cap
                    project_.dispatch_threads = n;
                }
                if (cJSON* k = cJSON_GetObjectItem(par, "queue_depth");
                    k && cJSON_IsNumber(k)) {
                    int n = (int)k->valuedouble;
                    if (n < 1)     n = 1;
                    if (n > 10000) n = 10000;
                    project_.queue_depth = n;
                }
                if (cJSON* k = cJSON_GetObjectItem(par, "overflow");
                    k && cJSON_IsString(k) && k->valuestring) {
                    std::string s = k->valuestring;
                    if (s == "drop_oldest" || s == "drop_newest" || s == "block") {
                        project_.overflow = s;
                    } else {
                        std::fprintf(stderr,
                            "[xinsp2] project.json parallelism.overflow "
                            "unknown value '%s' — using drop_oldest\n",
                            s.c_str());
                    }
                }
                if (cJSON* k = cJSON_GetObjectItem(par, "result_order");
                    k && cJSON_IsString(k) && k->valuestring) {
                    std::string s = k->valuestring;
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
                if (cJSON* arr = cJSON_GetObjectItem(par, "groups"); arr && cJSON_IsArray(arr)) {
                    auto warn = [&](const std::string& who, const std::string& msg) {
                        last_open_warnings_.push_back({who, "", msg});
                        std::fprintf(stderr, "[xinsp2] parallelism.groups: %s — %s\n", who.c_str(), msg.c_str());
                    };
                    cJSON* g = nullptr;
                    cJSON_ArrayForEach(g, arr) {
                        if (!cJSON_IsObject(g)) continue;
                        ProjectInfo::DispatchGroup grp;
                        if (cJSON* k = cJSON_GetObjectItem(g, "name"); k && cJSON_IsString(k) && k->valuestring) grp.name = k->valuestring;
                        if (grp.name.empty()) { warn("(unnamed)", "group missing 'name' — skipped"); continue; }
                        if (project_.find_group(grp.name)) { warn(grp.name, "duplicate group name — skipped"); continue; }  // #7
                        if (cJSON* k = cJSON_GetObjectItem(g, "max_parallel"); k && cJSON_IsNumber(k))
                            grp.max_parallel = std::min(32, std::max(1, (int)k->valuedouble));   // #4 clamp [1,32]
                        if (cJSON* k = cJSON_GetObjectItem(g, "thread_priority"); k && cJSON_IsString(k) && k->valuestring) {
                            grp.thread_priority = k->valuestring;
                            if (grp.thread_priority != "high" && grp.thread_priority != "normal" && grp.thread_priority != "low") {
                                warn(grp.name, "unknown thread_priority '" + grp.thread_priority + "' — using normal");
                                grp.thread_priority = "normal";
                            }
                        }
                        if (cJSON* k = cJSON_GetObjectItem(g, "queue_depth"); k && cJSON_IsNumber(k))
                            grp.queue_depth = std::min(10000, std::max(1, (int)k->valuedouble));
                        if (cJSON* k = cJSON_GetObjectItem(g, "overflow"); k && cJSON_IsString(k) && k->valuestring) {
                            grp.overflow = k->valuestring;
                            if (grp.overflow != "drop_oldest" && grp.overflow != "drop_newest" && grp.overflow != "block") {
                                warn(grp.name, "unknown overflow '" + grp.overflow + "' — using drop_oldest");
                                grp.overflow = "drop_oldest";
                            }
                        }
                        if (cJSON* k = cJSON_GetObjectItem(g, "result_order"); k && cJSON_IsString(k) && k->valuestring) {
                            grp.result_order = k->valuestring;
                            if (grp.result_order != "completion" && grp.result_order != "arrival") {
                                warn(grp.name, "unknown result_order '" + grp.result_order + "' — using completion");
                                grp.result_order = "completion";
                            }
                        }
                        if (cJSON* k = cJSON_GetObjectItem(g, "min_interval_ms"); k && cJSON_IsNumber(k))
                            grp.min_interval_ms = std::min(3600000, std::max(0, (int)k->valuedouble));
                        // cpu_affinity: flat [0,1,..] = one shared mask; nested
                        // [[..],[..]] = per-worker masks. Empty/invalid → unbound.
                        if (cJSON* k = cJSON_GetObjectItem(g, "cpu_affinity"); k && cJSON_IsArray(k)) {
                            auto parse_set = [&](cJSON* arr) {
                                std::vector<int> s;
                                cJSON* e = nullptr;
                                cJSON_ArrayForEach(e, arr) {
                                    if (!cJSON_IsNumber(e)) continue;
                                    int c = (int)e->valuedouble;
                                    if (c >= 0 && c < 1024) s.push_back(c);
                                    else warn(grp.name, "cpu_affinity core " + std::to_string(c) + " out of range — ignored");
                                }
                                return s;
                            };
                            cJSON* first = cJSON_GetArrayItem(k, 0);
                            if (first && cJSON_IsArray(first)) {          // nested: per-worker
                                cJSON* row = nullptr;
                                cJSON_ArrayForEach(row, k)
                                    if (cJSON_IsArray(row)) { auto s = parse_set(row); if (!s.empty()) grp.cpu_affinity.push_back(std::move(s)); }
                            } else {                                       // flat: one shared mask
                                auto s = parse_set(k);
                                if (!s.empty()) grp.cpu_affinity.push_back(std::move(s));
                            }
                        }
                        project_.groups.push_back(std::move(grp));
                    }
                    if (cJSON* k = cJSON_GetObjectItem(par, "default_group"); k && cJSON_IsString(k) && k->valuestring)
                        project_.default_group = k->valuestring;
                    if (project_.default_group.empty() && !project_.groups.empty())
                        project_.default_group = project_.groups.front().name;
                    if (!project_.default_group.empty() && !project_.find_group(project_.default_group))  // #6
                        warn(project_.default_group, "default_group names no declared group — falling back to '" +
                             (project_.groups.empty() ? std::string("(none)") : project_.groups.front().name) + "'");
                }
            }
            cJSON_Delete(root);
        } else {
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
                // 0/absent = unlimited). cJSON for the numeric field.
                if (cJSON* iroot = cJSON_Parse(ic.c_str())) {
                    if (cJSON* k = cJSON_GetObjectItem(iroot, "max_concurrency");
                        k && cJSON_IsNumber(k) && k->valuedouble > 0) {
                        ii.max_concurrency = (int)k->valuedouble;
                    }
                    cJSON_Delete(iroot);
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
                            if (!plugin_abi_compatible(pi2.handle, *plugin, &err)) {
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
