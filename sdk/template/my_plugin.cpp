//
// my_plugin.cpp — starting point for a new xInsp2 plugin.
//
// 1. Rename the file, class, and everything in plugin.json + CMakeLists.txt.
// 2. Override the virtuals you need (process/exchange/get_def/set_def).
// 3. Build, drop the output DLL + plugin.json + ui/ into
//    <xInsp2>/plugins/<name>/
//

#include <xi/xi_abi.hpp>

#include <string>

class MyPlugin : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Called per frame. Read from input, return a Record.
    xi::Record process(const xi::Record& input) override {
        // Example: echo an input field + compute something
        int in_val = input["value"].as_int(0);
        xi::Record out;
        out.set("doubled", in_val * 2);
        out.set("threshold", threshold_);
        return out;
    }

    // Called when the UI sends a command. Return the current state as JSON.
    std::string exchange(const std::string& cmd) override {
        cJSON* p = cJSON_Parse(cmd.c_str());
        if (p) {
            cJSON* c = cJSON_GetObjectItem(p, "command");
            cJSON* v = cJSON_GetObjectItem(p, "value");
            if (c && cJSON_IsString(c)) {
                std::string command = c->valuestring;
                if (command == "set_threshold" && v && cJSON_IsNumber(v)) {
                    threshold_ = (int)v->valuedouble;
                }
            }
            cJSON_Delete(p);
        }
        return get_def();
    }

    // Serialize config → JSON (saved in instance.json on project save).
    std::string get_def() const override {
        char buf[256];
        std::snprintf(buf, sizeof(buf), R"({"threshold":%d})", threshold_);
        return buf;
    }

    // Restore config ← JSON (called on instance create / project load).
    bool set_def(const std::string& json) override {
        cJSON* p = cJSON_Parse(json.c_str());
        if (!p) return false;
        cJSON* t = cJSON_GetObjectItem(p, "threshold");
        if (t && cJSON_IsNumber(t)) threshold_ = (int)t->valuedouble;
        cJSON_Delete(p);
        return true;
    }

private:
    int threshold_ = 128;
};

XI_PLUGIN_IMPL(MyPlugin)
