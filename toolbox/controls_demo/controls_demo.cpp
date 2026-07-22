// controls_demo — a real plugin whose def surface IS the controls pluginlet
// (toolbox/pluginlets/controls/controls.hpp, doc 37). It declares its parameters once as a
// UI tree and DELEGATES get_def/set_def to xi::pluginlet::Controls; the backend
// then serves the $schema + validated values with no hand-written marshalling.
// exchange("tick") bumps a counter and reflects it into a readout, to show
// plugin-pushed outputs flowing back through the same surface.
#include <xi/xi_abi.hpp>          // xi::Plugin, XI_PLUGIN_IMPL
#include <xi/xi_json.hpp>         // exchange command parse
#include <controls/controls.hpp> // xi::pluginlet::Controls

#include <string>

class ControlsDemo : public xi::Plugin {
public:
    ControlsDemo(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {
        ctl_.tab("Capture")
              .title("Demo source")
              .grid(12)
                .slider("fps", 30, 1, 60).caption("Frame rate").span(6)
                .numpad("gain", 1.0, 0.1, 4.0).caption("Gain").span(6)
                .toggle("streaming", true).caption("Streaming").span(6)
                .enumsel("mode", "fast", {"fast", "accurate"}).caption("Mode").span(6)
                .radio("trigger", "hardware", {"soft", "hardware", "free"}).caption("Trigger").span(12)
            .tab("Output")
              .grid(12)
                .readout("ticks",   "Ticks").span(6)
                .readout("last_fps","FPS in use").span(6)
              .divider()
              .grid(12)
                .button("reset", "Reset").span(12);
        ctl_.set_readout("ticks", "0");
        ctl_.set_readout("last_fps", "-");
    }

    // The whole def surface is the pluginlet — no hand-written JSON here.
    std::string get_def() const override        { return ctl_.get_def(); }
    bool        set_def(const std::string& j) override { return ctl_.set_def(j); }

    std::string exchange(const std::string& cmd_json) override {
        auto p = xi::Json::parse(cmd_json);
        const std::string c = p["command"].as_string();
        if (c == "tick") {
            auto s = ctl_.snapshot();                    // thread-safe read
            ctl_.set_readout("ticks", std::to_string(++ticks_));
            ctl_.set_readout("last_fps", std::to_string(s.i("fps")));
        } else if (c == "reset") {
            ticks_ = 0;
            ctl_.set_readout("ticks", "0");
            ctl_.set_readout("last_fps", "-");
        }
        return ctl_.get_def();
    }

private:
    xi::pluginlet::Controls ctl_;
    int ticks_ = 0;
};

XI_PLUGIN_IMPL(ControlsDemo)
