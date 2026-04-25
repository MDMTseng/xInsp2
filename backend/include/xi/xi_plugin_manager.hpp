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

struct PluginInfo {
    std::string name;
    std::string description;
    std::string dll_name;
    std::string factory_symbol;
    bool        has_ui = false;
    std::string folder_path;   // absolute path to plugin folder
    std::string ui_path;       // absolute path to ui/ folder (if has_ui)
    HMODULE     handle = nullptr;

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
          dll_(dll), inst_(inst) {
        // Resolve function pointers
        exchange_fn_ = reinterpret_cast<xi_plugin_exchange_fn>(GetProcAddress(dll_, "xi_plugin_exchange"));
        get_def_fn_  = reinterpret_cast<xi_plugin_get_def_fn>(GetProcAddress(dll_, "xi_plugin_get_def"));
        set_def_fn_  = reinterpret_cast<xi_plugin_set_def_fn>(GetProcAddress(dll_, "xi_plugin_set_def"));
        destroy_fn_  = reinterpret_cast<xi_plugin_destroy_fn>(GetProcAddress(dll_, "xi_plugin_destroy"));
        process_fn_  = reinterpret_cast<xi_plugin_process_fn>(GetProcAddress(dll_, "xi_plugin_process"));
    }

    ~CAbiInstanceAdapter() override {
        if (destroy_fn_ && inst_) destroy_fn_(inst_);
    }

    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return plugin_name_; }

    std::string get_def() const override {
        if (!get_def_fn_ || !inst_) return "{}";
        std::vector<char> buf(4096);
        int n = get_def_fn_(inst_, buf.data(), (int)buf.size());
        if (n < 0) { buf.resize((size_t)(-n) + 1024); n = get_def_fn_(inst_, buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    bool set_def(const std::string& j) override {
        if (!set_def_fn_ || !inst_) return false;
        return set_def_fn_(inst_, j.c_str()) == 0;
    }

    std::string exchange(const std::string& cmd_json) override {
        if (!exchange_fn_ || !inst_) return "{}";
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
                existing->second.description = info.description;
                existing->second.has_ui      = info.has_ui;
                existing->second.ui_path     = info.ui_path;
                existing->second.folder_path = info.folder_path;
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
                "#include <xi/xi_image.hpp>\n"
                "#include <xi/xi_ops.hpp>\n\n"
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
        project_ = ProjectInfo{};
    }

    bool open_project(const std::string& folder) {
        std::lock_guard<std::mutex> lk(mu_);
        auto pj = std::filesystem::path(folder) / "project.json";
        if (!std::filesystem::exists(pj)) return false;

        // Unregister old instances from the global registries
        for (auto& [k, v] : project_.instances) {
            InstanceRegistry::instance().remove(k);
            InstanceFolderRegistry::instance().clear(k);
        }
        project_.folder_path = folder;
        project_.instances.clear();

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
        // have none and we default to Any).
        project_.trigger_policy    = TriggerPolicy::Any;
        project_.trigger_required.clear();
        project_.trigger_leader.clear();
        project_.trigger_window_ms = 100;
        auto tp_pos = content.find("\"trigger_policy\"");
        if (tp_pos != std::string::npos) {
            // Parse just the small block — simplest: find enclosing { }
            auto bs = content.find('{', tp_pos);
            auto be = (bs == std::string::npos) ? std::string::npos : content.find('}', bs);
            if (bs != std::string::npos && be != std::string::npos) {
                std::string block = content.substr(bs, be - bs + 1);
                auto pol = extract_string(block, "policy");
                if (pol) {
                    if      (*pol == "all_required")     project_.trigger_policy = TriggerPolicy::AllRequired;
                    else if (*pol == "leader_followers") project_.trigger_policy = TriggerPolicy::LeaderFollowers;
                }
                auto ld = extract_string(block, "leader");
                if (ld) project_.trigger_leader = *ld;
                auto wp = block.find("\"window_ms\":");
                if (wp != std::string::npos) {
                    try { project_.trigger_window_ms = std::stoi(block.substr(wp + 12)); }
                    catch (...) {}
                }
                auto rp = block.find("\"required\":[");
                if (rp != std::string::npos) {
                    auto re = block.find(']', rp);
                    if (re != std::string::npos) {
                        std::string arr = block.substr(rp + 12, re - (rp + 12));
                        size_t pos = 0;
                        while (pos < arr.size()) {
                            auto q1 = arr.find('"', pos); if (q1 == std::string::npos) break;
                            auto q2 = arr.find('"', q1 + 1); if (q2 == std::string::npos) break;
                            project_.trigger_required.emplace_back(arr.substr(q1 + 1, q2 - q1 - 1));
                            pos = q2 + 1;
                        }
                    }
                }
            }
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
                    if (pi.c_factory) {
                        static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); return a; }();
                        void* raw = pi.c_factory(&host, ii.name.c_str());
                        if (raw) {
                            ii.instance = std::make_shared<CAbiInstanceAdapter>(
                                ii.name, *plugin, pi.handle, raw);
                            created = true;
                        }
                    } else if (pi.factory) {
                        auto* raw = pi.factory(ii.name.c_str());
                        if (raw) {
                            ii.instance.reset(raw);
                            created = true;
                        }
                    }
                    if (created && ii.instance) {
                        std::string cfg_val;
                        if (detail_find_key(ic, "config", cfg_val)) {
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
            // New C ABI — create via host API
            static xi_host_api host = []{ auto a = ImagePool::make_host_api(); install_trigger_hook(a); return a; }();
            void* raw = pi.c_factory(&host, instance_name.c_str());
            if (!raw) {
                InstanceFolderRegistry::instance().clear(instance_name);
                return nullptr;
            }
            ii.instance = std::make_shared<CAbiInstanceAdapter>(
                instance_name, plugin_name, pi.handle, raw);
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
            out += "{\"name\":\"" + v.name + "\",\"description\":\"" + v.description + "\"";
            out += ",\"has_ui\":" + std::string(v.has_ui ? "true" : "false");
            out += ",\"loaded\":" + std::string(v.handle ? "true" : "false") + "}";
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

    void save_project_locked() {
        auto pj = std::filesystem::path(project_.folder_path) / "project.json";
        std::string out = "{\n";
        out += "  \"name\": \""   + project_.name + "\",\n";
        out += "  \"script\": \"" + std::filesystem::path(project_.script_path).filename().string() + "\",\n";
        out += "  \"trigger_policy\": " + trigger_policy_json_locked() + ",\n";
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
