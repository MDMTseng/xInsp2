#pragma once
//
// xi_abi.hpp — C++ wrappers for the stable C ABI.
//
// Plugin authors include this and write normal C++ with Record objects.
// The XI_PLUGIN_IMPL macro generates the C ABI entry points automatically.
//
// Usage in a plugin:
//
//   class MyPlugin : public xi::PluginBase {
//   public:
//       MyPlugin(const std::string& name) : xi::PluginBase(name, "my_plugin") {}
//
//       Record process(const Record& input) override {
//           auto img = input.get_image("frame");
//           auto result = doSomething(img);
//           return Record()
//               .image("output", result)
//               .set("score", 0.95);
//       }
//   };
//
//   XI_PLUGIN_IMPL(MyPlugin)
//   // ^ generates: xi_plugin_create, xi_plugin_destroy, xi_plugin_process, etc.
//

#include "xi_abi.h"
#include "xi_record.hpp"

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace xi {

// --- Conversion helpers: C ↔ C++ ---

// Convert a C xi_record_ref to a C++ Record (copies image data)
inline Record record_from_ref(const xi_record_ref* ref) {
    Record r;
    if (ref->json_data) {
        cJSON* parsed = cJSON_Parse(ref->json_data);
        if (parsed) {
            // Iterate and copy fields
            cJSON* item = parsed->child;
            while (item) {
                if (cJSON_IsNumber(item))
                    r.set(item->string, item->valuedouble);
                else if (cJSON_IsBool(item))
                    r.set(item->string, cJSON_IsTrue(item) ? true : false);
                else if (cJSON_IsString(item))
                    r.set(item->string, std::string(item->valuestring));
                else {
                    char* s = cJSON_PrintUnformatted(item);
                    r.set_raw(item->string, cJSON_Duplicate(item, true));
                    cJSON_free(s);
                }
                item = item->next;
            }
            cJSON_Delete(parsed);
        }
    }
    for (int i = 0; i < ref->image_count; ++i) {
        auto& img = ref->images[i];
        r.image(img.key, Image(img.width, img.height, img.channels, img.data));
    }
    return r;
}

// Convert a C++ Record to a C xi_record_out (copies image data)
inline void record_to_out(const Record& r, xi_record_out* out) {
    xi_record_out_init(out);

    // JSON data
    std::string json = r.data_json();
    xi_record_out_set_json(out, json.c_str());

    // Images
    for (auto& [key, img] : r.images()) {
        if (!img.empty() && img.data()) {
            xi_record_out_add_image(out, key.c_str(),
                                     img.data(), img.width, img.height, img.channels);
        }
    }
}

// Build a xi_record_ref (borrowed pointers) from a Record.
// The ref is valid as long as the Record lives. No copies.
struct RecordRefHolder {
    std::vector<xi_image_ref> img_refs;
    std::string json_str;
    xi_record_ref ref;

    explicit RecordRefHolder(const Record& r) {
        json_str = r.data_json();
        for (auto& [key, img] : r.images()) {
            xi_image_ref ir;
            ir.key      = key.c_str();
            ir.data     = img.data();
            ir.width    = img.width;
            ir.height   = img.height;
            ir.channels = img.channels;
            ir.stride   = img.stride();
            img_refs.push_back(ir);
        }
        ref.images      = img_refs.data();
        ref.image_count = (int32_t)img_refs.size();
        ref.json_data   = json_str.c_str();
    }
};

// --- PluginBase — C++ base class for plugin authors ---

class PluginBase {
public:
    PluginBase(std::string name, std::string plugin_name)
        : name_(std::move(name)), plugin_name_(std::move(plugin_name)) {}

    virtual ~PluginBase() = default;

    const std::string& name() const { return name_; }
    const std::string& pluginName() const { return plugin_name_; }

    // Override this to implement your plugin's processing.
    virtual Record process(const Record& input) { (void)input; return {}; }

    // Override for UI commands (start, stop, set_fps, etc.)
    virtual std::string exchange(const std::string& cmd_json) { (void)cmd_json; return "{}"; }

    // Override for persistence
    virtual std::string get_def() const { return "{}"; }
    virtual bool set_def(const std::string& json) { (void)json; return true; }

protected:
    std::string name_;
    std::string plugin_name_;
};

} // namespace xi

//
// XI_PLUGIN_IMPL(ClassName) — generates all C ABI entry points.
//
// Put this at the bottom of your plugin .cpp file after defining
// your PluginBase subclass.
//
#define XI_PLUGIN_IMPL(ClassName)                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
void* xi_plugin_create(const char* instance_name) {                            \
    return new ClassName(instance_name);                                        \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
void xi_plugin_destroy(void* instance) {                                       \
    delete static_cast<ClassName*>(instance);                                   \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
void xi_plugin_process(void* instance,                                         \
                       const xi_record_ref* input,                             \
                       xi_record_out* output) {                                \
    auto* self = static_cast<ClassName*>(instance);                            \
    xi::Record in_rec = xi::record_from_ref(input);                            \
    xi::Record out_rec = self->process(in_rec);                                \
    xi::record_to_out(out_rec, output);                                        \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_exchange(void* instance,                                         \
                       const char* cmd_json,                                   \
                       char* rsp_buf, int rsp_buflen) {                        \
    auto* self = static_cast<ClassName*>(instance);                            \
    std::string rsp = self->exchange(cmd_json);                                \
    int needed = (int)rsp.size();                                              \
    if (rsp_buflen < needed + 1) return -needed;                               \
    std::memcpy(rsp_buf, rsp.data(), rsp.size());                              \
    rsp_buf[rsp.size()] = 0;                                                   \
    return needed;                                                             \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_get_def(void* instance, char* buf, int buflen) {                 \
    auto* self = static_cast<ClassName*>(instance);                            \
    std::string def = self->get_def();                                         \
    int needed = (int)def.size();                                              \
    if (buflen < needed + 1) return -needed;                                   \
    std::memcpy(buf, def.data(), def.size());                                  \
    buf[def.size()] = 0;                                                       \
    return needed;                                                             \
}                                                                              \
                                                                               \
extern "C" __declspec(dllexport)                                               \
int xi_plugin_set_def(void* instance, const char* json) {                      \
    auto* self = static_cast<ClassName*>(instance);                            \
    return self->set_def(json) ? 0 : -1;                                       \
}
