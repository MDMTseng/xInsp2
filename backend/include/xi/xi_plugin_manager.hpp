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
#include "xi_image_pool.hpp"
#include "xi_instance.hpp"

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
};

class PluginManager {
public:
    // Scan a directory for plugin folders. Each subfolder with a plugin.json
    // is registered.
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
            plugins_[info.name] = std::move(info);
            count++;
        }
        return count;
    }

    // Load a plugin's DLL and resolve the factory function.
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

        // Try new C ABI first (xi_host_api*, const char*) → void*
        pi.c_factory = reinterpret_cast<PluginInfo::CFactoryFn>(
            GetProcAddress(pi.handle, pi.factory_symbol.c_str()));
        // Try old-style factory as fallback
        if (!pi.c_factory) {
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

        project_.name = name;
        project_.folder_path = folder;
        project_.script_path = (std::filesystem::path(folder) / "inspection.cpp").string();
        project_.instances.clear();

        // Write initial project.json
        save_project_locked();

        // Create a starter inspection.cpp if it doesn't exist
        if (!std::filesystem::exists(project_.script_path)) {
            std::ofstream f(project_.script_path);
            f << "// " << name << " — inspection script\n"
              << "#include <xi/xi.hpp>\n"
              << "#include <xi/xi_image.hpp>\n"
              << "#include <xi/xi_ops.hpp>\n\n"
              << "XI_SCRIPT_EXPORT\n"
              << "void xi_inspect_entry(int frame) {\n"
              << "    // TODO: add inspection logic\n"
              << "}\n";
        }
        return true;
    }

    bool open_project(const std::string& folder) {
        std::lock_guard<std::mutex> lk(mu_);
        auto pj = std::filesystem::path(folder) / "project.json";
        if (!std::filesystem::exists(pj)) return false;

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

        // Scan instances/ subdirectories
        auto inst_dir = std::filesystem::path(folder) / "instances";
        if (std::filesystem::exists(inst_dir)) {
            for (auto& entry : std::filesystem::directory_iterator(inst_dir)) {
                if (!entry.is_directory()) continue;
                auto ij = entry.path() / "instance.json";
                if (!std::filesystem::exists(ij)) continue;
                std::ifstream ijf(ij.string());
                std::stringstream iss;
                iss << ijf.rdbuf();
                std::string ic = iss.str();
                auto plugin = extract_string(ic, "plugin");
                if (!plugin) continue;

                InstanceInfo ii;
                ii.name = entry.path().filename().string();
                ii.plugin_name = *plugin;
                ii.folder_path = entry.path().string();
                // Try to create the instance from the plugin factory
                auto pit = plugins_.find(*plugin);
                if (pit != plugins_.end() && pit->second.factory) {
                    auto* raw = pit->second.factory(ii.name.c_str());
                    if (raw) {
                        ii.instance.reset(raw);
                        // Load saved config
                        auto def_opt = extract_string(ic, "config");
                        // Actually read full config object
                        auto cfg_pos = ic.find("\"config\":");
                        if (cfg_pos != std::string::npos) {
                            // Extract the JSON value after "config":
                            std::string cfg_val;
                            const char* after;
                            if (detail_find_key(ic, "config", cfg_val)) {
                                ii.instance->set_def(cfg_val);
                            }
                        }
                        InstanceRegistry::instance().add(ii.instance);
                    }
                }
                project_.instances[ii.name] = std::move(ii);
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

        if (pi.c_factory) {
            // New C ABI — create via host API
            static xi_host_api host = ImagePool::make_host_api();
            void* raw = pi.c_factory(&host, instance_name.c_str());
            if (!raw) return nullptr;
            ii.instance = std::make_shared<CAbiInstanceAdapter>(
                instance_name, plugin_name, pi.handle, raw);
        } else {
            // Old-style factory
            auto* raw = pi.factory(instance_name.c_str());
            if (!raw) return nullptr;
            ii.instance.reset(raw);
        }
        InstanceRegistry::instance().add(ii.instance);

        // Save instance.json
        save_instance_json(ii);

        project_.instances[instance_name] = std::move(ii);
        save_project_locked();
        return &project_.instances[instance_name];
    }

    // Save an instance's current config to its folder.
    bool save_instance(const std::string& instance_name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = project_.instances.find(instance_name);
        if (it == project_.instances.end()) return false;
        save_instance_json(it->second);
        return true;
    }

    ProjectInfo& project() { return project_; }

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

private:
    std::mutex mu_;
    std::unordered_map<std::string, PluginInfo> plugins_;
    ProjectInfo project_;

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
        std::ofstream f(pj.string());
        f << "{\n";
        f << "  \"name\": \"" << project_.name << "\",\n";
        f << "  \"script\": \"" << std::filesystem::path(project_.script_path).filename().string() << "\",\n";
        f << "  \"instances\": [";
        int i = 0;
        for (auto& [k, v] : project_.instances) {
            if (i++) f << ",";
            f << "\n    {\"name\": \"" << v.name << "\", \"plugin\": \"" << v.plugin_name << "\"}";
        }
        f << "\n  ]\n}\n";
    }

    void save_instance_json(const InstanceInfo& ii) {
        auto path = std::filesystem::path(ii.folder_path) / "instance.json";
        std::ofstream f(path.string());
        f << "{\n";
        f << "  \"plugin\": \"" << ii.plugin_name << "\",\n";
        f << "  \"config\": ";
        if (ii.instance) f << ii.instance->get_def();
        else             f << "{}";
        f << "\n}\n";
    }
};

} // namespace xi
