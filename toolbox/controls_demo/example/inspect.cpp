// controls_demo example — a plugin whose ENTIRE config UI is declared in C++.
//
// Open the `ctl` instance's UI: the tabs, the 12-column grid, the sliders /
// numpad / toggle / enum / radio, the readouts and the Reset button are not
// hand-written HTML or JSON. They are the `controls` pluginlet's builder calls
// in toolbox/controls_demo/controls_demo.cpp — the plugin declares its surface
// once and the webui renders it (docs/new_gen/37-pluginlet-model.md).
//
// This script just ticks the plugin so the readouts move while you watch, and
// reads the live control values back through the same surface. Drag the "Frame
// rate" slider and the "FPS in use" readout follows on the next run.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>

XI_INSPECT_ENTRY(t, frame) {
    (void)t; (void)frame;

    // exchange("tick") bumps the demo's counter and reflects the CURRENT value
    // of the `fps` control into the "FPS in use" readout. The reply is the
    // plugin's whole def surface — the same JSON the UI renders from, so it is
    // also how a script reads back what the operator set.
    const std::string def = xi::use("ctl").exchange(R"({"command":"tick"})");

    if (def.empty()) { xi::ng(2, "ctl did not answer — is the instance up?"); return; }
    // The def carries the control tree; a crude probe is enough here (the point
    // of the demo is the UI, not JSON parsing — a real script would use
    // xi::Json). "ticks" is the readout the tick just updated.
    if (def.find("ticks") == std::string::npos) {
        xi::ng(2, "def surface has no 'ticks' readout — controls plet not wired?");
        return;
    }
    xi::ok(1, "ticked — watch the readouts in the ctl UI");
}
