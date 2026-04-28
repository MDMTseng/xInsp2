#pragma once
//
// xi_plugin_handle.hpp — generic proxy for calling any plugin through
// the C ABI without needing the plugin's header/class definition.
//
// Usage in a user script:
//
//   xi::PluginHandle blobs{"detector0", "blob_analysis"};
//
//   void xi_inspect_entry(int frame) {
//       auto frame = cam->grab();
//       auto result = blobs.process(xi::Record()
//           .image("frame", frame)
//           .set("threshold", 128));
//       VAR(detection, result);
//   }
//
// PluginHandle is an InstanceBase subclass so it registers in the
// InstanceRegistry and works with xi::Instance<> if needed.
//
// Under the hood it:
//   1. LoadLibrary's the plugin DLL (found by name in the plugins/ dir)
//   2. Resolves xi_plugin_create, xi_plugin_process, etc.
//   3. Creates an instance via the C ABI factory
//   4. process() marshals Record → xi_record → call → xi_record_out → Record
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
#include "xi_record.hpp"
#include "xi_instance.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace xi {

class PluginHandle : public InstanceBase {
public:
    PluginHandle(const std::string& instance_name, const std::string& plugin_name)
        : name_(instance_name), plugin_name_(plugin_name) {
        // Auto-discover and load the plugin DLL
        if (!find_and_load(plugin_name)) {
            fprintf(stderr, "[PluginHandle] failed to load plugin: %s\n", plugin_name.c_str());
        }
    }

    ~PluginHandle() override {
        if (instance_ && destroy_fn_) destroy_fn_(instance_);
        if (dll_) FreeLibrary(dll_);
    }

    PluginHandle(const PluginHandle&) = delete;
    PluginHandle& operator=(const PluginHandle&) = delete;

    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return plugin_name_; }

    bool loaded() const { return instance_ != nullptr && process_fn_ != nullptr; }

    // --- Core: call the plugin's process() ---
    Record process(const Record& input) {
        if (!instance_ || !process_fn_) return {};

        // Marshal: Record → C ABI input
        std::vector<xi_record_image> in_imgs;
        std::vector<xi_image_handle> in_handles;
        for (auto& [key, img] : input.images()) {
            xi_image_handle h = ImagePool::instance().from_image(img);
            in_handles.push_back(h);
            in_imgs.push_back({key.c_str(), h});
        }
        std::string json = input.data_json();
        xi_record in_rec;
        in_rec.images = in_imgs.data();
        in_rec.image_count = (int32_t)in_imgs.size();
        in_rec.json = json.c_str();

        // Call
        xi_record_out out;
        xi_record_out_init(&out);
        process_fn_(instance_, &in_rec, &out);

        // Release input handles
        for (auto h : in_handles) ImagePool::instance().release(h);

        // Unmarshal: C ABI output → Record
        Record result;
        if (out.json) {
            cJSON* parsed = cJSON_Parse(out.json);
            if (parsed) {
                cJSON* item = parsed->child;
                while (item) {
                    if (cJSON_IsNumber(item))      result.set(item->string, item->valuedouble);
                    else if (cJSON_IsBool(item))   result.set(item->string, cJSON_IsTrue(item) ? true : false);
                    else if (cJSON_IsString(item)) result.set(item->string, std::string(item->valuestring));
                    else                           result.set_raw(item->string, cJSON_Duplicate(item, true));
                    item = item->next;
                }
                cJSON_Delete(parsed);
            }
        }
        for (int i = 0; i < out.image_count; ++i) {
            result.image(out.images[i].key, ImagePool::instance().to_image(out.images[i].handle));
            ImagePool::instance().release(out.images[i].handle);
        }
        xi_record_out_free(&out);
        return result;
    }

    // --- InstanceBase interface ---
    std::string exchange(const std::string& cmd_json) override {
        if (!instance_ || !exchange_fn_) return "{}";
        std::vector<char> buf(64 * 1024);
        int n = exchange_fn_(instance_, cmd_json.c_str(), buf.data(), (int)buf.size());
        if (n < 0) { buf.resize((size_t)(-n) + 1024);
                     n = exchange_fn_(instance_, cmd_json.c_str(), buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    std::string get_def() const override {
        if (!instance_ || !get_def_fn_) return "{}";
        std::vector<char> buf(4096);
        int n = get_def_fn_(instance_, buf.data(), (int)buf.size());
        if (n < 0) { buf.resize((size_t)(-n) + 1024);
                     n = get_def_fn_(instance_, buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    bool set_def(const std::string& j) override {
        if (!instance_ || !set_def_fn_) return false;
        return set_def_fn_(instance_, j.c_str()) == 0;
    }

private:
    std::string name_;
    std::string plugin_name_;
    HMODULE     dll_ = nullptr;
    void*       instance_ = nullptr;

    xi_plugin_create_fn  create_fn_  = nullptr;
    xi_plugin_destroy_fn destroy_fn_ = nullptr;
    xi_plugin_process_fn process_fn_ = nullptr;
    xi_plugin_exchange_fn exchange_fn_ = nullptr;
    xi_plugin_get_def_fn  get_def_fn_ = nullptr;
    xi_plugin_set_def_fn  set_def_fn_ = nullptr;

    xi_host_api host_;

    bool find_and_load(const std::string& plugin_name) {
        // Search for the plugin DLL in common locations
        std::vector<std::string> search_paths;

        // Relative to the exe
        char exe_path[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        auto exe_dir = std::filesystem::path(exe_path).parent_path();

        // Walk up to find plugins/ dir
        auto p = exe_dir;
        for (int i = 0; i < 6; ++i) {
            auto candidate = p / "plugins" / plugin_name / (plugin_name + ".dll");
            if (std::filesystem::exists(candidate)) {
                search_paths.push_back(candidate.string());
                break;
            }
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }

        // Try each path
        for (auto& dll_path : search_paths) {
            dll_ = LoadLibraryA(dll_path.c_str());
            if (dll_) break;
        }
        if (!dll_) return false;

        // Resolve symbols
        create_fn_  = (xi_plugin_create_fn) GetProcAddress(dll_, "xi_plugin_create");
        destroy_fn_ = (xi_plugin_destroy_fn)GetProcAddress(dll_, "xi_plugin_destroy");
        process_fn_ = (xi_plugin_process_fn)GetProcAddress(dll_, "xi_plugin_process");
        exchange_fn_ = (xi_plugin_exchange_fn)GetProcAddress(dll_, "xi_plugin_exchange");
        get_def_fn_  = (xi_plugin_get_def_fn) GetProcAddress(dll_, "xi_plugin_get_def");
        set_def_fn_  = (xi_plugin_set_def_fn) GetProcAddress(dll_, "xi_plugin_set_def");

        if (!create_fn_ || !process_fn_) {
            FreeLibrary(dll_);
            dll_ = nullptr;
            return false;
        }

        // Create the host API and the instance
        host_ = ImagePool::make_host_api();
        instance_ = create_fn_(&host_, name_.c_str());
        return instance_ != nullptr;
    }
};

// Factory specialization so xi::Instance<PluginHandle> works — but
// PluginHandle needs a plugin_name which Instance<T> can't provide.
// Instead, use PluginHandle directly:
//
//   PluginHandle blobs{"detector0", "blob_analysis"};
//   auto result = blobs.process(input);

} // namespace xi
