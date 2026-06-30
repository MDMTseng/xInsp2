//
// inspect.cpp — qa_recipe_script_instance.
//
// Declares ONE script-owned xi::Instance<Scaler>. Scaler holds a single tunable
// "factor" (default 5) exposed through get_def/set_def. The driver tunes it,
// captures the recipe via list_instances, resets, then load_project's the recipe
// back and asserts the TUNED factor was re-applied — the regression test for the
// load_project script-instance drop (a fail-reads-as-pass: recipe loads green
// while the script instance silently kept its default).
//
#include <xi/xi.hpp>

// A trivial script-declared plugin holding one tunable int.
class Scaler : public xi::InstanceBase {
public:
    explicit Scaler(std::string n) : name_(std::move(n)) {}
    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return "Scaler"; }

    std::string get_def() const override {
        return "{\"factor\":" + std::to_string(factor_) + "}";
    }
    bool set_def(const std::string& j) override {
        auto pos = j.find("\"factor\":");
        if (pos == std::string::npos) return false;
        factor_ = std::stoi(j.substr(pos + 9));
        return true;
    }
    // Echo current def so the driver can read it back without scraping a VAR.
    std::string exchange(const std::string& /*cmd*/) override { return get_def(); }

private:
    std::string name_;
    int factor_ = 5;
};

namespace xi {
template <>
std::shared_ptr<Scaler> make_plugin_instance<Scaler>(std::string_view name) {
    return std::make_shared<Scaler>(std::string(name));
}
}

static xi::Instance<Scaler> scaler{"scaler"};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Touch the instance so it stays live; no wire output needed.
    scaler->exchange("{}");
    (void)frame;
}
