//
// inspect.cpp — hue_tune, orchestration only.
//
// One project plugin, one instance "det" (hue_counter). The driver
// changes its config between runs via exchange_instance — no
// recompile here, no recompile of the plugin.
//
// Per-run fields pushed to the `expose` plugin (channel "hue"):
//   input       (image, original RGB)
//   mask        (image, 1-channel binary)
//   count       (int)
//   hue_lo      (int)  — band lo used for this run
//   hue_hi      (int)  — band hi used for this run
//   min_area    (int)
//

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto path = xi::current_frame_path();
    if (path.empty()) {
        xi::use("expose").process(xi::Record()
            .set("$channel", "error")
            .set("error", std::string("no frame_path supplied to cmd:run")));
        return;
    }

    auto frame = xi::imread(path);
    if (frame.empty()) {
        xi::use("expose").process(xi::Record()
            .set("$channel", "error")
            .set("error", std::string("frame load failed: ") + path));
        return;
    }

    auto& det = xi::use("det");
    auto out  = det.process(xi::Record().image("src", frame));

    // Surface the input, mask + counts through the `expose` plugin.
    xi::use("expose").process(xi::Record()
        .set("$channel", "hue")
        .image("input",      frame)
        .set("frame_path",   path)
        .image("mask",       out.get_image("mask"))
        .set("count",        out["count"].as_int(-1))
        .set("hue_lo",       out["hue_lo"].as_int(-1))
        .set("hue_hi",       out["hue_hi"].as_int(-1))
        .set("min_area",     out["min_area"].as_int(-1)));
}
