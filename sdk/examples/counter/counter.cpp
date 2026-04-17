//
// counter.cpp — state, persistence, and UI.
//
// Demonstrates:
//   - holding state across frames
//   - serializing state with get_def / set_def so it survives
//     hot-reload + project save/load
//   - a webview UI that triggers commands and displays status
//

#include <xi/xi_abi.hpp>

#include <cstdio>
#include <string>

class Counter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Each frame: increment and emit the count.
    xi::Record process(const xi::Record& /*input*/) override {
        ++count_;
        return xi::Record().set("count", count_);
    }

    // UI sends: {command: "reset"} or {command: "set_step", value: N} or {command: "get_status"}
    std::string exchange(const std::string& cmd) override {
        cJSON* p = cJSON_Parse(cmd.c_str());
        if (p) {
            cJSON* c = cJSON_GetObjectItem(p, "command");
            cJSON* v = cJSON_GetObjectItem(p, "value");
            if (c && cJSON_IsString(c)) {
                std::string command = c->valuestring;
                if      (command == "reset")     count_ = 0;
                else if (command == "set_step" && v && cJSON_IsNumber(v))
                                                 step_ = (int)v->valuedouble;
            }
            cJSON_Delete(p);
        }
        return get_def();
    }

    // Serialize state → JSON (persisted to instance.json)
    std::string get_def() const override {
        char buf[128];
        std::snprintf(buf, sizeof(buf), R"({"count":%d,"step":%d})", count_, step_);
        return buf;
    }

    // Restore state ← JSON
    bool set_def(const std::string& json) override {
        cJSON* p = cJSON_Parse(json.c_str());
        if (!p) return false;
        if (auto* n = cJSON_GetObjectItem(p, "count")) count_ = (int)n->valuedouble;
        if (auto* n = cJSON_GetObjectItem(p, "step"))  step_  = (int)n->valuedouble;
        cJSON_Delete(p);
        return true;
    }

private:
    int count_ = 0;
    int step_  = 1;
};

XI_PLUGIN_IMPL(Counter)
