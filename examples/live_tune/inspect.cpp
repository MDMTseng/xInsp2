//
// inspect.cpp — live_tune.
//
// Drives the project's single instance ("det" / circle_counter) and
// surfaces its `count` plus the cleaned mask. The plugin is mis-tuned
// out of the gate (over-aggressive erosion); the agent edits the plugin
// source and calls `cmd:recompile_project_plugin` to hot-reload until
// every frame reports count == 8.
//
// Frame is loaded from the path passed by the driver via
// `c.run(frame_path=...)`, available here as `xi::current_frame_path()`.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    std::string fpath = xi::current_frame_path();
    xi::Image frame = xi::imread(fpath);
    if (frame.empty()) {
        VAR(error,      std::string("imread failed: ") + fpath);
        VAR(frame_path, fpath);
        return;
    }
    VAR(input,      frame);
    VAR(frame_path, fpath);

    auto& det = xi::use("det");
    auto out = det.process(xi::Record().image("src", frame));
    if (out.has("error")) {
        VAR(error, out["error"].as_string("detector error"));
        return;
    }

    int count_v = out["count"].as_int(0);
    VAR(count, count_v);
    VAR(mask,  out.get_image("mask"));
}
