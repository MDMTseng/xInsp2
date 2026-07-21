// data_output example — a plugin's CONFIG SURFACE, and how it fails honestly.
//
// Read this one for what it teaches, not for what it writes: data_output models
// the config surface of a results writer (output_dir / format / auto_save) and
// deliberately does NOT implement `save`. Asking it to save returns the
// framework's structured error shape instead of a success-looking answer.
//
//   >>> For real persistence use the record_save plugin. <<<
//
// Two things worth copying from here into your own plugin:
//   * a plugin's def surface is readable AND writable from a script, so config
//     is not UI-only state;
//   * an unimplemented verb answers with xi::contract::fault_json — a caller
//     can branch on `error`, which "" or a stale def would never let it do.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>

XI_INSPECT_ENTRY(t, frame) {
    (void)t; (void)frame;

    // 1. Read the config the operator sees in the instance UI.
    const std::string def = xi::use("out").exchange(R"({"command":"get_status"})");
    if (def.find("output_dir") == std::string::npos) {
        xi::ng(2, "out did not answer with its config surface");
        return;
    }

    // 2. Ask it to do the thing it does not do. The point is the SHAPE of the
    //    refusal: {"error":"not_implemented","key":"save"} — machine-checkable.
    const std::string save = xi::use("out").exchange(R"({"command":"save"})");
    const bool refused_honestly = save.find("not_implemented") != std::string::npos;

    if (!refused_honestly) {
        // If this ever trips, the stub grew an implementation (or started lying)
        // — the example, and this comment, need updating.
        xi::ng(1, "save did not refuse with not_implemented — surface changed?");
        return;
    }
    xi::ok(1, "config surface read; save refused honestly (use record_save)");
}
