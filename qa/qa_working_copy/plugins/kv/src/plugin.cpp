//
// kv.cpp — stores a single string "value" via get_def/set_def. A minimal
// stateful instance for the working-copy persistence test (qa/qa_working_copy):
// the driver edits the value, saves the instance, and checks where it lands
// (scratch vs canonical) across commit / crash-resume / discard.
//
#include <xi/xi.hpp>
#include <xi/xi_json.hpp>
#include <string>

class Kv : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    void process(xi::PackIn& /*in*/, xi::PackOut& out) override {
        out.str("value", value_);
    }

    std::string get_def() const override {
        return xi::Json::object().set("value", value_).dump();
    }
    bool set_def(const std::string& j) override {
        auto p = xi::Json::parse(j);
        if (!p.valid()) return false;
        value_ = p["value"].as_string(value_);
        return true;
    }

private:
    std::string value_ = "original";
};

XI_PLUGIN_IMPL(Kv)
XI_PLUGIN_PACK_DOOR(Kv)
