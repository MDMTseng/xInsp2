//
// inspect.cpp — crash_recovery2 orchestration.
//
// Loads the per-run frame_path supplied by c.run(frame_path=...), feeds
// it to the "det" instance (count_or_crash plugin), and surfaces:
//   - count       : the blob count (int)
//   - crashed     : 1 if the plugin's per-call error was set, else 0
//   - error       : the error string when crashed
//   - mask, input : preview images
//
// The whole point of this script is that even when det.process() crashes,
// the script returns normally — the crash was absorbed by the worker
// process and replied as a Record with `error` set.
//

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    auto path = xi::current_frame_path();
    if (path.empty()) {
        VAR(error, std::string("no frame_path supplied to cmd:run"));
        VAR(crashed, int(0));
        VAR(count, int(-1));
        return;
    }

    auto frame = xi::imread(path);
    if (frame.empty()) {
        VAR(error, std::string("frame load failed: ") + path);
        VAR(crashed, int(0));
        VAR(count, int(-1));
        return;
    }
    VAR(input, frame);
    VAR(frame_path, path);

    auto& det = xi::use("det");
    auto out = det.process(xi::Record().image("src", frame));

    auto err = out["error"].as_string("");
    if (!err.empty()) {
        VAR(error, err);
        VAR(crashed, int(1));
        VAR(count, int(-1));
        return;
    }

    VAR(crashed, int(0));
    VAR(count, out["count"].as_int(-1));
    auto mask_img = out.get_image("mask");
    if (!mask_img.empty()) {
        VAR(mask, mask_img);
    }
}
