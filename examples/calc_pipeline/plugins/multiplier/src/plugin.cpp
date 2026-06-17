// multiplier — multiplies input["value"] by a configurable `factor`. No image processing.
//
// Pure data plugin (xi::Record JSON only). `factor` round-trips through
// get_def/set_def, so it persists in instance.json and is retunable live.

#include <xi/xi_abi.hpp>   // xi::Plugin, xi::Record, XI_PLUGIN_IMPL

class Multiplier : public xi::Plugin {
    double factor_ = 1.0;   // applied to every input; loaded from instance config
public:
    using xi::Plugin::Plugin;

    // --- config (instances/<name>/instance.json "config") ---
    std::string get_def() const override {
        return std::string("{\"factor\":") + std::to_string(factor_) + "}";
    }
    bool set_def(const std::string& json) override {
        auto cfg = xi::Record::from_json_bytes(
            reinterpret_cast<const uint8_t*>(json.data()), json.size());
        factor_ = cfg["factor"].as_double(factor_);
        return true;
    }

    // --- the work: result = value * factor ---
    xi::Record process(const xi::Record& in) override {
        double value = in["value"].as_double(0.0);
        return xi::Record()
            .set("result", value * factor_)
            .set("op", std::string("multiply"))
            .set("factor", factor_);
    }
};

XI_PLUGIN_IMPL(Multiplier)
