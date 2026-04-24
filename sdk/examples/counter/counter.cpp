//
// counter.cpp — state, persistence, and UI.
//
// Demonstrates:
//   - holding state across frames
//   - serializing state with get_def / set_def so it survives
//     hot-reload + project save/load
//   - a webview UI that triggers commands and displays status
//
// Uses xi::Json (RAII cJSON wrapper) — the canonical way to parse
// commands and build replies. See sdk/README.md §xi::Json cheatsheet.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

class Counter : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Each frame: increment and emit the count.
    xi::Record process(const xi::Record& /*input*/) override {
        count_ += step_;
        return xi::Record().set("count", count_);
    }

    // UI sends:
    //   {command: "reset"}
    //   {command: "set_step", value: N}
    //   {command: "get_status"}   (or anything unknown — falls through)
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "reset")     count_ = 0;
        else if (command == "set_step")  step_ = p["value"].as_int(step_);
        return get_def();
    }

    // Serialize state → JSON (persisted to instance.json)
    std::string get_def() const override {
        return xi::Json::object()
            .set("count", count_)
            .set("step",  step_)
            .dump();
    }

    // Restore state ← JSON
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        count_ = p["count"].as_int(count_);
        step_  = p["step"].as_int(step_);
        return true;
    }

private:
    int count_ = 0;
    int step_  = 1;
};

XI_PLUGIN_IMPL(Counter)
