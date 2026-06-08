// Phase-B smoke source — now via xi::Emitter. Per exchange it emits one frame:
// the Emitter mints the res_id, assigns a contiguous seq, stages the image, and
// calls emit_resource + emit_dispatch (bypassing the trigger bus). The script
// reads the id back via current_trigger().id_string() and pulls the frame.
#include <xi/xi.hpp>
#include <xi/xi_emitter.hpp>
class DispSrc : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    std::string exchange(const std::string& /*cmd*/) override {
        em_.bind(host(), name());
        em_.image("img", pool_image(5, 2, 1));
        em_.emit();
        return "{}";
    }
private:
    xi::Emitter em_;
};
XI_PLUGIN_IMPL(DispSrc)
