// Minimal source for the H6 test: on any exchange command it emits ONE trigger
// with XI_TRIGGER_NULL (so the bus mints a fresh id per emit). Under an
// all_required policy two such sources can never correlate -> the bus must warn.
#include <xi/xi.hpp>
class NullPulse : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& /*cmd*/) override {
        xi::Image img = pool_image(1, 1, 1);
        xi_record_image rec{};
        rec.key = "img";
        rec.handle = img.pool_handle();
        if (host() && host()->emit_trigger)
            host()->emit_trigger(name().c_str(), XI_TRIGGER_NULL, 0, &rec, 1);
        return "{}";
    }
};
XI_PLUGIN_IMPL(NullPulse)
