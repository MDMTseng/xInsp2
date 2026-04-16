//
// user_with_instance.cpp — M6 test script with Instance<T> and Param<T>.
//

#include <xi/xi.hpp>
#include <xi/xi_image.hpp>

// A trivial plugin that holds a multiplier and a name.
class Scaler : public xi::InstanceBase {
public:
    explicit Scaler(std::string n) : name_(std::move(n)) {}
    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return "Scaler"; }

    std::string get_def() const override {
        return "{\"factor\":" + std::to_string(factor_) + "}";
    }
    bool set_def(const std::string& j) override {
        // Parse "factor" from JSON. Minimal.
        auto pos = j.find("\"factor\":");
        if (pos == std::string::npos) return false;
        factor_ = std::stoi(j.substr(pos + 9));
        return true;
    }

    int apply(int v) const { return v * factor_; }

private:
    std::string name_;
    int factor_ = 2;
};

namespace xi {
template <>
std::shared_ptr<Scaler> make_plugin_instance<Scaler>(std::string_view name) {
    return std::make_shared<Scaler>(std::string(name));
}
}

static xi::Instance<Scaler> scaler{"my_scaler"};
static xi::Param<int> base{"base_val", 10, {1, 100}};

extern "C" __declspec(dllexport)
void xi_inspect_entry(int frame) {
    VAR(input,  frame * static_cast<int>(base));
    VAR(scaled, scaler->apply(input));
    VAR(tag,    std::string("instance_test"));
}
