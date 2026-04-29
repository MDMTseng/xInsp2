//
// inspect.cpp — crash_recovery, single-stage script.
//
// Loads the frame named in c.run(frame_path=...), feeds it to the
// "cnt" instance of count_or_crash. Either gets back a {count, mask}
// pair, or the plugin segfaults inside its (worker) process and the
// framework absorbs it. The driver checks the post-call backend
// liveness; this script just plumbs the call through.
//

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto path = xi::current_frame_path();
    if (path.empty()) {
        VAR(error, std::string("no frame_path supplied to cmd:run"));
        return;
    }

    auto frame = xi::imread(path);
    if (frame.empty()) {
        VAR(error, std::string("frame load failed: ") + path);
        return;
    }
    VAR(input,      frame);
    VAR(frame_path, path);

    auto& cnt = xi::use("cnt");
    auto out  = cnt.process(xi::Record().image("src", frame));

    // Either we got back {count, mask}, or the plugin crashed and the
    // framework returned an error record / nothing. The driver decides.
    VAR(count, out["count"].as_int(-1));
    if (!out.get_image("mask").empty()) {
        VAR(mask, out.get_image("mask"));
    }
    auto err = out["error"].as_string("");
    if (!err.empty()) {
        VAR(error, err);
    }
}
