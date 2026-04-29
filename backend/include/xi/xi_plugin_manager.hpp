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
#include "xi_cert.hpp"
#include "xi_image_pool.hpp"
#include "xi_instance.hpp"
#include "xi_process_instance.hpp"
#include "xi_script_compiler.hpp"
#include "xi_source.hpp"
#include "xi_trigger_bus.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
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
                        HMODULE dll, void* inst)
        : name_(std::move(name)), plugin_name_(std::move(plugin_name)),
          dll_(dll), inst_(inst),
          owner_id_(ImagePool::alloc_owner_id()) {
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
        std::vector<char> buf(4096);
        int n = get_def_fn_(inst_, buf.data(), (int)buf.size());
        if (n < 0) { buf.resize((size_t)(-n) + 1024); n = get_def_fn_(inst_, buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    bool set_def(const std::string& j) override {
        if (!set_def_fn_ || !inst_) return false;
        ImagePool::OwnerGuard g(owner_id_);
        return set_def_fn_(inst_, j.c_str()) == 0;
    }

    std::string exchange(const std::string& cmd_json) override {
        if (!exchange_fn_ || !inst_) return "{}";
        ImagePool::OwnerGuard g(owner_id_);
        std::vector<char> buf(64 * 1024);
        int n = exchange_fn_(inst_, cmd_json.c_str(), buf.data(), (int)buf.size());
        if (n < 0) { buf.resize((size_t)(-n) + 1024); n = exchange_fn_(inst_, cmd_json.c_str(), buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    void* raw_instance() const { return inst_; }
    xi_plugin_process_fn process_fn() const { return process_fn_; }

private:
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

struct InstanceInfo {
    std::string          name;
    std::string          plugin_name;
    std::string          folder_path;  // project/instances/<name>/
    std::shared_ptr<InstanceBase> instance;
};

struct ProjectInfo {
    std::string name;
    std::string folder_path;
    std::string script_path;
    std::vector<std::string> plugin_names;  // which plugins are used
    std::unordered_map<std::string, InstanceInfo> instances;

    // Trigger bus policy persisted per project. Default is Any (every
    // source emit fires one dispatch) — back-compat with pre-trigger
    // plugins. Change to AllRequired for multi-camera synchronisation.
    TriggerPolicy trigger_policy    = TriggerPolicy::Any;
    std::vector<std::string> trigger_required;    // source names (AllRequired)
    std::string   trigger_leader;                 // source name (LeaderFollowers)
    int           trigger_window_ms = 100;

    // `parallelism.dispatch_threads`: how many dispatcher threads
    // run xi_inspect_entry concurrently in continuous mode.
    // Default 1 — single threaded, current behaviour, watchdog
    // active. >1 fans out the trigger queue + timer ticks across
    // N threads. Caveats (xi::state, plugin process() reentrance,
    // watchdog disabled) — see writing-a-script.md.
    int           dispatch_threads = 1;

    // `parallelism.queue_depth`: maximum number of trigger events
    // (real bus emits + synthetic timer ticks) buffered while
    // workers are busy. Default 100 — enough to absorb a normal
    // burst without runaway memory if a source overproduces.
    int           queue_depth      = 100;

    // `parallelism.overflow`: what to do when queue_depth is full
    // and another event arrives.
    //   "drop_oldest" (default) — pop the front, push the new one.
    //                            Live inspection prefers freshest.
    //   "drop_newest"           — keep the FIFO ordering, refuse
    //                            the new event. Use for archival /
    //                            ML logging where order matters.
    //   "block"                 — emit_trigger blocks until room is
    //                            available. Back-pressure to source.
    std::string   overflow         = "drop_oldest";
};

// Static compile environment for project-level plugins. Populated once at
// backend startup so PluginManager doesn't need to know about service-main
// globals; mirrors the fields service_main feeds into xi::script::compile
// for inspection scripts.
struct CompileEnv {
    std::string include_dir;
    std::string vcvars_path;
    std::string opencv_dir;
    std::string turbojpeg_root;
    std::string ipp_root;
};

class PluginManager {
public:
    void set_compile_env(const CompileEnv& env) {
        std::lock_guard<std::mutex> lk(mu_);
        compile_env_ = env;
    }

    // Where xinsp-worker.exe lives + the name of the backend's SHM
    // region. Both are passed to spawned workers so they can attach
    // and serve isolated instances. Empty values disable isolation —
    // instances declaring isolation:"process" then fall back to in-proc
    // with a warning.
    void set_isolation_env(std::filesystem::path worker_exe,
                           std::string shm_name) {
        std::lock_guard<std::mutex> lk(mu_);
        worker_exe_ = std::move(worker_exe);
        shm_name_   = std::move(shm_name);
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
                auto walk = [&](const std::filesystem::path& dir) {
                    for (auto& f : std::filesystem::directory_iterator(dir)) {
                        if (!f.is_regular_file()) continue;
                        auto ext = f.path().extension().string();
                        if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                            sources.push_back(f.path().string());
                        }
                    }
                };
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

                std::fprintf(stderr,
                    "[xinsp2] compiling project plugin '%s' (%zu source%s)...\n",
                    pname.c_str(), sources.size(), sources.size() == 1 ? "" : "s");
                auto res = xi::script::compile(req);
                if (!res.ok) {
                    last_open_warnings_.push_back(
                        {pname, pname, "compile failed (see Output for details)"});
                    std::fprintf(stderr,
                        "[xinsp2] project plugin '%s' compile FAILED:\n%s\n",
                        pname.c_str(), res.build_log.c_str());
                    continue;
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
                    if (pi.has_ui) pi.ui_path = (entry.path() / "ui").string();
                    std::string mblock;
                    if (detail_find_key(mc, "manifest", mblock)) pi.manifest_json = std::move(mblock);
                }
                // Load the freshly built DLL up-front. Project plugins
                // skip the cert/baseline gate — they are inside the
                // user's own project, not third-party code, so we trust
                // the source. (Export will run cert.)
                auto dll_path = std::filesystem::path(pi.folder_path) / pi.dll_name;
                pi.handle = LoadLibraryA(dll_path.string().c_str());
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
        };
        std::vector<Pending> pending;
        for (auto& [iname, ii] : project_.instances) {
            if (ii.plugin_name != plugin_name) continue;
            Pending p;
            p.name   = iname;
            p.folder = ii.folder_path;
            if (ii.instance) p.def_json = ii.instance->get_def();
            pending.push_back(std::move(p));
        }
        for (auto& p : pending) {
            auto& ii = project_.instances[p.name];
            InstanceRegistry::instance().remove(p.name);
            ii.instance.reset();   // dtor calls xi_plugin_destroy
        }

        // 2. Compile fresh into the same plugin folder. We don't drop
        //    the old DLL until the new one is ready, so a compile
        //    failure leaves the project in its previous working state.
        auto plugin_dir = std::filesystem::path(source_dir);
        std::vector<std::string> sources;
        auto walk = [&](const std::filesystem::path& dir) {
            for (auto& f : std::filesystem::directory_iterator(dir)) {
                if (!f.is_regular_file()) continue;
                auto ext = f.path().extension().string();
                if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                    sources.push_back(f.path().string());
                }
            }
        };
        auto src_subdir = plugin_dir / "src";
        if (std::filesystem::exists(src_subdir)) walk(src_subdir);
        else                                     walk(plugin_dir);
        if (sources.empty()) {
            r.error = "no .cpp sources in " + plugin_dir.string();
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
                static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); return a; }();
                for (auto& p : pending) {
                    void* raw = pi_it->second.c_factory(&host, p.name.c_str());
                    if (!raw) continue;
                    auto adapter = std::make_shared<CAbiInstanceAdapter>(
                        p.name, plugin_name, pi_it->second.handle, raw);
                    if (!p.def_json.empty()) adapter->set_def(p.def_json);
                    project_.instances[p.name].instance = adapter;
                    InstanceRegistry::instance().add(adapter);
                    attach_trigger_bridge(adapter.get(), p.name);
                    r.reattached_instances.push_back(p.name);
                }
            }
            return r;
        }

        // 3. Compile succeeded — swap DLLs.
        auto pi_it = plugins_.find(plugin_name);
        if (pi_it == plugins_.end()) {
            r.error = "internal: plugin entry vanished mid-recompile";
            return r;
        }
        auto& pi = pi_it->second;
        if (pi.handle) {
            FreeLibrary(pi.handle);
            pi.handle    = nullptr;
            pi.factory   = nullptr;
            pi.c_factory = nullptr;
        }
        pi.dll_name    = std::filesystem::path(cres.dll_path).filename().string();
        pi.folder_path = std::filesystem::path(cres.dll_path).parent_path().string();
        pi.handle = LoadLibraryA(cres.dll_path.c_str());
        if (!pi.handle) {
            r.error = "LoadLibrary failed on freshly-built DLL";
            return r;
        }
        {
            std::string err;
            if (!plugin_abi_compatible(pi.handle, plugin_name, &err)) {
                r.error = err;
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
            r.error = "factory '" + pi.factory_symbol + "' not exported in new DLL";
            return r;
        }

        // 4. Re-instantiate every preserved instance using the new factory.
        static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); return a; }();
        for (auto& p : pending) {
            std::shared_ptr<InstanceBase> inst;
            if (pi.c_factory) {
                void* raw = pi.c_factory(&host, p.name.c_str());
                if (raw) inst = std::make_shared<CAbiInstanceAdapter>(
                    p.name, plugin_name, pi.handle, raw);
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
        auto walk = [&](const std::filesystem::path& dir) {
            for (auto& f : std::filesystem::directory_iterator(dir)) {
                if (!f.is_regular_file()) continue;
                auto ext = f.path().extension().string();
                if (ext == ".cpp" || ext == ".cc" || ext == ".cxx") {
                    sources.push_back(f.path().string());
                }
            }
        };
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
    bool load_plugin(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = plugins_.find(name);
        if (it == plugins_.end()) return false;
        auto& pi = it->second;
        if (pi.handle) return true; // already loaded

        auto dll_path = std::filesystem::path(pi.folder_path) / pi.dll_name;
        if (!std::filesystem::exists(dll_path)) return false;

        pi.handle = LoadLibraryA(dll_path.string().c_str());
        if (!pi.handle) return false;

        {
            std::string err;
            if (!plugin_abi_compatible(pi.handle, name, &err)) {
                std::fprintf(stderr, "[xinsp2] %s\n", err.c_str());
                FreeLibrary(pi.handle);
                pi.handle = nullptr;
                return false;
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
                    return false;
                }
            }
        } else {
            // Old-style: factory takes (const char*) → InstanceBase*
            pi.factory = reinterpret_cast<PluginInfo::FactoryFn>(
                GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
        }
        return pi.c_factory != nullptr || pi.factory != nullptr;
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
        project_.script_path = (std::filesystem::path(folder) / "inspection.cpp").string();
        project_.instances.clear();

        // Write initial project.json
        save_project_locked();

        // Create a starter inspection.cpp if it doesn't exist
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
        // Drop project plugins entirely — their DLLs were built into the
        // closed project's build/ folder and won't be valid against a
        // different project. Global plugins stay registered.
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

    bool open_project(const std::string& folder) {
        std::lock_guard<std::mutex> lk(mu_);
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
        // ProcessInstanceAdapter's destructor closes a pipe + tears
        // down its worker process, no host DLL dependency, so order
        // doesn't matter for that branch — but doing it here keeps a
        // single deterministic teardown sequence.
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
        else            project_.script_path = (std::filesystem::path(folder) / "inspection.cpp").string();

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
            }
            cJSON_Delete(root);
        }
        TriggerBus::instance().set_policy(
            project_.trigger_policy, project_.trigger_required,
            project_.trigger_leader, project_.trigger_window_ms);

        // Compile project-local plugins BEFORE instances are loaded — the
        // instance loop below resolves plugin name → loaded DLL, and we
        // want project plugins to win over global ones with the same name.
        // last_open_warnings_ is reset here so compile failures + bad
        // instances both end up in the same surfaced list.
        last_open_warnings_.clear();
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
        bool warned_no_iso_env = false;
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
                // Auto-load the plugin if not yet loaded
                auto pit = plugins_.find(*plugin);
                if (pit != plugins_.end() && !pit->second.factory && !pit->second.c_factory) {
                    // Plugin discovered but not loaded — load it now
                    auto& pi2 = pit->second;
                    auto dll_path = std::filesystem::path(pi2.folder_path) / pi2.dll_name;
                    if (std::filesystem::exists(dll_path)) {
                        pi2.handle = LoadLibraryA(dll_path.string().c_str());
                        if (pi2.handle) {
                            auto has_destroy = GetProcAddress(pi2.handle, "xi_plugin_destroy") != nullptr;
                            if (has_destroy)
                                pi2.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
                                    GetProcAddress(pi2.handle, pi2.factory_symbol.c_str()));
                            else
                                pi2.factory = reinterpret_cast<PluginInfo::FactoryFn>(
                                    GetProcAddress(pi2.handle, pi2.factory_symbol.c_str()));
                        }
                    }
                }
                if (pit != plugins_.end()) {
                    auto& pi = pit->second;
                    bool created = false;
                    // Same registration as create_instance — needed for project-load too.
                    InstanceFolderRegistry::instance().set(ii.name, ii.folder_path);

                    // Default: isolation:"process" → spawn a xinsp-worker.exe
                    // per instance and proxy method calls over IPC. Pixel
                    // data goes via SHM so process() is zero-copy across
                    // the process boundary. A buggy plugin can crash its
                    // worker without taking the backend down; the manager
                    // auto-respawns it.
                    //
                    // Opt out per instance with `"isolation": "in_process"`
                    // when you want the plugin to share the backend's
                    // address space (debugger easier; lower per-call
                    // latency; higher blast radius on crash). The legacy
                    // value `"process"` keeps working as an explicit
                    // declaration of the default.
                    //
                    // If the isolation env isn't configured (the
                    // xinsp-worker.exe / SHM region weren't wired up at
                    // backend startup), every instance falls back to
                    // in-proc with a single warning per project open.
                    auto iso = extract_string(ic, "isolation");
                    bool isolation_env_ok = !worker_exe_.empty() && !shm_name_.empty();
                    bool want_isolated;
                    if (!iso) {
                        want_isolated = isolation_env_ok;
                    } else if (*iso == "in_process" || *iso == "none") {
                        want_isolated = false;
                    } else if (*iso == "process") {
                        want_isolated = true;
                    } else {
                        std::fprintf(stderr,
                            "[xinsp2] instance '%s' has unknown isolation value '%s' — using default (process)\n",
                            ii.name.c_str(), iso->c_str());
                        want_isolated = isolation_env_ok;
                    }

                    // Optional per-instance IPC call timeout. Honoured only
                    // for isolation:"process" instances — the in-proc path
                    // doesn't have a watchdog of its own (the script-level
                    // g_watchdog_ms covers it). Range: any positive int;
                    // typical values 5000..120000 ms. Missing / non-positive
                    // → adapter's built-in 30 s default.
                    int call_timeout_ms = 0;
                    if (auto tm = extract_string(ic, "call_timeout_ms")) {
                        try { call_timeout_ms = std::stoi(*tm); } catch (...) {}
                    }
                    if (want_isolated && isolation_env_ok) {
                        try {
                            auto dll_path = std::filesystem::path(pi.folder_path) / pi.dll_name;
                            auto adapter = std::make_shared<ProcessInstanceAdapter>(
                                ii.name, *plugin, worker_exe_, dll_path, shm_name_,
                                ii.folder_path);
                            if (call_timeout_ms > 0) adapter->set_call_timeout_ms(call_timeout_ms);
                            ii.instance = std::move(adapter);
                            created = true;
                        } catch (const std::exception& e) {
                            std::fprintf(stderr,
                                "[xinsp2] isolation:process spawn failed for '%s': %s — "
                                "falling back to in-proc\n",
                                ii.name.c_str(), e.what());
                            // fall through to in-proc path below
                        }
                    } else if (want_isolated && !isolation_env_ok) {
                        // Print this warning at most once per project open
                        // so a project with N instances doesn't get N
                        // identical lines.
                        if (!warned_no_iso_env) {
                            std::fprintf(stderr,
                                "[xinsp2] isolation env not configured (worker exe / shm) "
                                "— this project's plugins will run in-proc\n");
                            warned_no_iso_env = true;
                        }
                    }

                    if (!created && pi.c_factory) {
                        static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); return a; }();
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
                                ii.name, *plugin, pi.handle, raw);
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
                        last_open_warnings_.push_back(
                            {inst_name, *plugin, "factory returned null"});
                        std::fprintf(stderr,
                            "[xinsp2] skip instance '%s' (%s): factory returned null\n",
                            inst_name.c_str(), plugin->c_str());
                    }
                } else {
                    last_open_warnings_.push_back(
                        {inst_name, *plugin, "plugin not loaded / not found"});
                    std::fprintf(stderr,
                        "[xinsp2] skip instance '%s': plugin '%s' not loaded\n",
                        inst_name.c_str(), plugin->c_str());
                }
                project_.instances[ii.name] = std::move(ii);
                } catch (const std::exception& e) {
                    last_open_warnings_.push_back(
                        {inst_name, "", std::string("exception: ") + e.what()});
                    std::fprintf(stderr,
                        "[xinsp2] skip instance '%s': %s\n",
                        inst_name.c_str(), e.what());
                } catch (...) {
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
                                   const std::string& plugin_name) {
        std::lock_guard<std::mutex> lk(mu_);
        if (project_.folder_path.empty()) return nullptr;

        auto pit = plugins_.find(plugin_name);
        if (pit == plugins_.end()) return nullptr;
        auto& pi = pit->second;
        if (!pi.factory && !pi.c_factory) return nullptr;

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
            static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); return a; }();
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
                return nullptr;
            }
            auto adapter = std::make_shared<CAbiInstanceAdapter>(
                instance_name, plugin_name, pi.handle, raw);
            adapter->adopt_owner_id(pre_owner);
            ii.instance = std::move(adapter);
        } else {
            // Old-style factory
            auto* raw = pi.factory(instance_name.c_str());
            if (!raw) return nullptr;
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
            static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); return a; }();
            void* raw = pi.c_factory(&host, new_name.c_str());
            if (!raw) { InstanceFolderRegistry::instance().clear(new_name); return false; }
            ii.instance = std::make_shared<CAbiInstanceAdapter>(
                new_name, ii.plugin_name, pi.handle, raw);
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
        std::string out = "{\"name\":\"" + project_.name + "\"";
        out += ",\"script\":\"" + std::filesystem::path(project_.script_path).filename().string() + "\"";
        out += ",\"instances\":[";
        int i = 0;
        for (auto& [k, v] : project_.instances) {
            if (i++) out += ",";
            out += "{\"name\":\"" + v.name + "\",\"plugin\":\"" + v.plugin_name + "\"}";
        }
        out += "],\"plugins\":[";
        i = 0;
        for (auto& [k, v] : plugins_) {
            if (i++) out += ",";
            bool is_proj = project_plugin_origin_.count(v.name) > 0;
            out += "{\"name\":\"" + v.name + "\",\"description\":\"" + v.description + "\"";
            out += ",\"has_ui\":" + std::string(v.has_ui ? "true" : "false");
            out += ",\"loaded\":" + std::string(v.handle ? "true" : "false");
            // origin: "project" if compiled from <project>/plugins, else "global"
            out += ",\"origin\":\"" + std::string(is_proj ? "project" : "global") + "\"";
            if (is_proj) {
                out += ",\"source_dir\":\"";
                // escape backslashes for JSON
                for (char c : project_plugin_origin_[v.name]) {
                    if (c == '\\' || c == '"') out += '\\';
                    out += c;
                }
                out += "\"";
            }
            // Optional `manifest` block from plugin.json — passed through
            // verbatim. Clients (AI agents, doc tools) parse the body
            // themselves; the backend doesn't validate or reshape it.
            if (!v.manifest_json.empty()) {
                out += ",\"manifest\":" + v.manifest_json;
            }
            out += "}";
        }
        out += "]}";
        return out;
    }

    // Per-instance failure record from the most recent open_project. The
    // open succeeds even if individual instances fail (skip-bad-instance);
    // callers can read these to surface a warning to the user.
    struct OpenWarning {
        std::string instance;   // folder name; "" if we couldn't determine it
        std::string plugin;     // referenced plugin (may be empty/unknown)
        std::string reason;     // human-readable
    };
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
    // Set by set_isolation_env(); both must be non-empty for worker spawn.
    std::filesystem::path worker_exe_;
    std::string           shm_name_;
    // Names of plugins that came from <project>/plugins/ rather than the
    // global plugins directory — flagged so the UI can label them and so
    // we don't re-scan their dll mtime against the global cert.
    std::unordered_map<std::string, std::string> project_plugin_origin_;

    static PluginInfo parse_manifest(const std::string& path, const std::string& folder) {
        PluginInfo pi;
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string content = ss.str();

        auto name = extract_string(content, "name");
        auto desc = extract_string(content, "description");
        auto dll  = extract_string(content, "dll");
        auto fact = extract_string(content, "factory");

        if (name) pi.name = *name;
        if (desc) pi.description = *desc;
        if (dll)  pi.dll_name = *dll;
        else      pi.dll_name = pi.name + ".dll";
        if (fact) pi.factory_symbol = *fact;
        else      pi.factory_symbol = "xi_plugin_create";

        pi.has_ui = (content.find("\"has_ui\":true") != std::string::npos) ||
                    (content.find("\"has_ui\": true") != std::string::npos);
        pi.folder_path = folder;
        if (pi.has_ui) {
            pi.ui_path = (std::filesystem::path(folder) / "ui").string();
        }
        // Optional manifest block — preserved verbatim. Empty if absent.
        std::string m;
        if (detail_find_key(content, "manifest", m)) pi.manifest_json = std::move(m);
        return pi;
    }

    static std::optional<std::string> extract_string(const std::string& json,
                                                      const std::string& key) {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return std::nullopt;
        pos = json.find(':', pos);
        if (pos == std::string::npos) return std::nullopt;
        pos++;
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos >= json.size() || json[pos] != '"') return std::nullopt;
        pos++; // skip opening quote
        auto end = json.find('"', pos);
        if (end == std::string::npos) return std::nullopt;
        return json.substr(pos, end - pos);
    }

    static bool detail_find_key(const std::string& json, const std::string& key,
                                 std::string& out) {
        auto pos = json.find("\"" + key + "\":");
        if (pos == std::string::npos) return false;
        pos += key.size() + 3;
        while (pos < json.size() && json[pos] == ' ') pos++;
        // Extract value (object, array, string, or scalar)
        const char* p = json.data() + pos;
        const char* end = json.data() + json.size();
        const char* start = p;
        if (*p == '{' || *p == '[') {
            char open = *p, close = (open == '{') ? '}' : ']';
            int depth = 0;
            while (p < end) {
                if (*p == '"') { p++; while (p < end && *p != '"') { if (*p == '\\') p++; p++; } }
                if (*p == open) depth++;
                if (*p == close) { depth--; if (depth == 0) { p++; break; } }
                p++;
            }
        } else if (*p == '"') {
            p++;
            while (p < end && *p != '"') { if (*p == '\\') p++; p++; }
            p++;
        } else {
            while (p < end && *p != ',' && *p != '}' && *p != ']') p++;
        }
        out.assign(start, p);
        return true;
    }

    // FL r6 P2-3: validate an instance's `config` JSON against the
    // plugin's `manifest.params` declarations. Emits one OpenWarning per
    // unknown key / type-mismatch / out-of-range / not-in-enum value.
    //
    // Validation is best-effort and warnings-only: a bad value still
    // gets passed to `Plugin::set_def`, which already silently falls
    // back to its compiled-in default for unknown / unparseable fields.
    // The warning is the user-visible signal that a typo or stale
    // value made it through.
    //
    // Back-compat: if `manifest_json` is empty, doesn't parse, or
    // doesn't contain a `params` array, validation is skipped without
    // warnings. Plugins predating manifests stay silent.
    //
    // Pure C++ / cJSON; no platform calls. Safe to share across
    // open_project() invocations.
    static void validate_config_against_manifest(
        const std::string& instance,
        const std::string& plugin,
        const std::string& config_json,
        const std::string& manifest_json,
        std::vector<OpenWarning>& out_warnings)
    {
        if (manifest_json.empty()) return;
        cJSON* mroot = cJSON_Parse(manifest_json.c_str());
        if (!mroot) return;
        cJSON* params = cJSON_GetObjectItem(mroot, "params");
        if (!params || !cJSON_IsArray(params)) {
            cJSON_Delete(mroot);
            return;
        }
        cJSON* croot = cJSON_Parse(config_json.c_str());
        if (!croot || !cJSON_IsObject(croot)) {
            if (croot) cJSON_Delete(croot);
            cJSON_Delete(mroot);
            return;
        }

        // Build a quick name -> param-decl index. The manifest is small
        // (a few params) so a linear scan would also be fine.
        std::unordered_map<std::string, cJSON*> by_name;
        cJSON* it = nullptr;
        cJSON_ArrayForEach(it, params) {
            if (!cJSON_IsObject(it)) continue;
            cJSON* nm = cJSON_GetObjectItem(it, "name");
            if (nm && cJSON_IsString(nm) && nm->valuestring) {
                by_name[nm->valuestring] = it;
            }
        }

        auto type_of_default = [](cJSON* decl) -> const char* {
            // Prefer explicit "type" if declared; else infer from the
            // "default" value's JSON type. Returns one of:
            // "int", "float", "bool", "string", "" (unknown).
            if (cJSON* t = cJSON_GetObjectItem(decl, "type");
                t && cJSON_IsString(t) && t->valuestring) {
                return t->valuestring;
            }
            cJSON* d = cJSON_GetObjectItem(decl, "default");
            if (!d) return "";
            if (cJSON_IsBool(d))   return "bool";
            if (cJSON_IsString(d)) return "string";
            if (cJSON_IsNumber(d)) {
                // Best-effort split of int vs float based on the literal.
                double v = d->valuedouble;
                if (v == (double)(long long)v) return "int";
                return "float";
            }
            return "";
        };

        auto value_matches_type = [](cJSON* v, const std::string& t) -> bool {
            if (t == "int" || t == "float" || t == "number") {
                return cJSON_IsNumber(v) != 0;
            }
            if (t == "bool" || t == "boolean") {
                return cJSON_IsBool(v) != 0;
            }
            if (t == "string") {
                return cJSON_IsString(v) != 0;
            }
            // Unknown type tag — don't false-positive.
            return true;
        };

        cJSON* cv = nullptr;
        cJSON_ArrayForEach(cv, croot) {
            if (!cv->string) continue;
            const std::string key = cv->string;
            auto pit = by_name.find(key);
            if (pit == by_name.end()) {
                out_warnings.push_back({
                    instance, plugin,
                    "unknown_config_key: '" + key +
                    "' is not declared in plugin manifest.params"
                });
                continue;
            }
            cJSON* decl = pit->second;
            std::string declared_type = type_of_default(decl);
            if (!declared_type.empty() &&
                !value_matches_type(cv, declared_type)) {
                out_warnings.push_back({
                    instance, plugin,
                    "type_mismatch: config['" + key +
                    "'] does not match declared type '" + declared_type + "'"
                });
                // Don't bother with min/max/enum if the type is wrong.
                continue;
            }

            // Numeric range check (min / max).
            if (cJSON_IsNumber(cv)) {
                double v = cv->valuedouble;
                cJSON* mn = cJSON_GetObjectItem(decl, "min");
                cJSON* mx = cJSON_GetObjectItem(decl, "max");
                if (mn && cJSON_IsNumber(mn) && v < mn->valuedouble) {
                    out_warnings.push_back({
                        instance, plugin,
                        "out_of_range: config['" + key + "'] = " +
                        std::to_string(v) + " is below declared min " +
                        std::to_string(mn->valuedouble)
                    });
                }
                if (mx && cJSON_IsNumber(mx) && v > mx->valuedouble) {
                    out_warnings.push_back({
                        instance, plugin,
                        "out_of_range: config['" + key + "'] = " +
                        std::to_string(v) + " is above declared max " +
                        std::to_string(mx->valuedouble)
                    });
                }
            }

            // Enum check for strings — declared as a JSON array under
            // "enum". Membership is exact-string.
            if (cJSON_IsString(cv) && cv->valuestring) {
                cJSON* en = cJSON_GetObjectItem(decl, "enum");
                if (en && cJSON_IsArray(en)) {
                    bool found = false;
                    std::string allowed;
                    cJSON* eit = nullptr;
                    cJSON_ArrayForEach(eit, en) {
                        if (cJSON_IsString(eit) && eit->valuestring) {
                            if (!allowed.empty()) allowed += ", ";
                            allowed += "'";
                            allowed += eit->valuestring;
                            allowed += "'";
                            if (std::string(eit->valuestring) == cv->valuestring) {
                                found = true;
                            }
                        }
                    }
                    if (!found) {
                        out_warnings.push_back({
                            instance, plugin,
                            "not_in_enum: config['" + key + "'] = '" +
                            cv->valuestring + "' is not in declared enum {" +
                            allowed + "}"
                        });
                    }
                }
            }
            // TODO(p2-3-extend): structured object/array params — current
            // schema only declares scalar params. When manifest.params
            // grows nested-object support, extend the recursion here.
        }

        cJSON_Delete(croot);
        cJSON_Delete(mroot);
    }

    void save_project_locked() {
        auto pj = std::filesystem::path(project_.folder_path) / "project.json";
        std::string out = "{\n";
        out += "  \"name\": \""   + project_.name + "\",\n";
        out += "  \"script\": \"" + std::filesystem::path(project_.script_path).filename().string() + "\",\n";
        out += "  \"trigger_policy\": " + trigger_policy_json_locked() + ",\n";
        // Preserve `parallelism` block. Earlier versions only wrote
        // name / script / trigger_policy / instances, so any UI flow
        // that called cmd:save_project silently dropped the user's
        // dispatch_threads / queue_depth / overflow config. Always
        // write the block — defaults round-trip cleanly.
        out += "  \"parallelism\": {";
        out += "\"dispatch_threads\":" + std::to_string(project_.dispatch_threads);
        out += ",\"queue_depth\":"     + std::to_string(project_.queue_depth);
        out += ",\"overflow\":\""      + project_.overflow + "\"";
        out += "},\n";
        out += "  \"instances\": [";
        int i = 0;
        for (auto& [k, v] : project_.instances) {
            if (i++) out += ",";
            out += "\n    {\"name\": \"" + v.name + "\", \"plugin\": \"" + v.plugin_name + "\"}";
        }
        out += "\n  ]\n}\n";
        xi::atomic_write(pj, out);
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
            s += "\""; s += project_.trigger_required[i]; s += "\"";
        }
        s += "],\"leader\":\"";
        s += project_.trigger_leader;
        s += "\"}";
        return s;
    }

    void save_instance_json(const InstanceInfo& ii) {
        auto path = std::filesystem::path(ii.folder_path) / "instance.json";
        std::string out = "{\n";
        out += "  \"plugin\": \"" + ii.plugin_name + "\",\n";
        out += "  \"config\": ";
        out += ii.instance ? ii.instance->get_def() : "{}";
        out += "\n}\n";
        xi::atomic_write(path, out);
    }
};

} // namespace xi
