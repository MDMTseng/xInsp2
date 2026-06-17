// adder — adds a configurable `addend` to input["value"]. No image processing.
//
// Pure data plugin: it only reads/writes xi::Record (JSON) fields. `addend`
// round-trips through get_def/set_def, so it persists in instance.json and can
// be retuned live in the instance UI.

#include <xi/xi_abi.hpp>   // xi::Plugin, xi::Record, XI_PLUGIN_IMPL

class Adder : public xi::Plugin {
    double addend_ = 0.0;   // applied to every input; loaded from instance config
public:
    using xi::Plugin::Plugin;

    // --- config (instances/<name>/instance.json "config") ---
    std::string get_def() const override {
        return std::string("{\"addend\":") + std::to_string(addend_) + "}";
    }
    bool set_def(const std::string& json) override {
        auto cfg = xi::Record::from_json_bytes(
            reinterpret_cast<const uint8_t*>(json.data()), json.size());
        addend_ = cfg["addend"].as_double(addend_);
        return true;
    }

    // --- the work: result = value + addend ---
    xi::Record process(const xi::Record& in) override {
        double value = in["value"].as_double(0.0);
        return xi::Record()
            .set("result", value + addend_)
            .set("op", std::string("add"))
            .set("addend", addend_);
    }
};

XI_PLUGIN_IMPL(Adder)
