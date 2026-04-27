//
// inspect.cpp — golden-reference defect detection, orchestration only.
//
// Plugins do all the image math:
//   - "ref"    (reference_image)       : holds the cached golden image
//   - "src"    (png_frame_source)      : loads frame_NN.png by idx
//   - "finder" (golden_defect_finder)  : |frame-ref| → blur → threshold
//                                        → close → regions → bbox
//
// The driver loop sets `frame_idx` between runs.
// Reference is loaded ONCE at project-open via instance.json (or
// reloaded at runtime through the reference_image plugin's UI).
//

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <string>

static xi::Param<int> frame_idx{"frame_idx", 0, {0, 9999}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    int idx = frame_idx;

    auto& ref    = xi::use("ref");
    auto& src    = xi::use("src");
    auto& finder = xi::use("finder");

    // 1. Pull the cached reference. No file I/O — just hands back the
    //    Image already sitting in the plugin instance's state.
    auto ref_out = ref.process(xi::Record{});
    bool ref_loaded = ref_out["loaded"].as_bool(false);
    VAR(reference_loaded, ref_loaded);
    if (!ref_loaded) {
        VAR(error,          ref_out["error"].as_string("reference not loaded"));
        VAR(defect_present, false);
        VAR(score,          0.0);
        return;
    }
    auto ref_img = ref_out.get_image("reference");
    VAR(reference, ref_img);

    // 2. Load this frame.
    auto src_out = src.process(xi::Record().set("idx", idx));
    if (!src_out["loaded"].as_bool(false)) {
        VAR(error,          src_out["error"].as_string("frame load failed"));
        VAR(defect_present, false);
        VAR(score,          0.0);
        return;
    }
    auto frame = src_out.get_image("frame");
    VAR(input,      frame);
    VAR(frame_path, src_out["path"].as_string(""));

    // 3. Compare.
    auto out = finder.process(xi::Record()
        .image("reference", ref_img)
        .image("frame",     frame));

    VAR(diff,           out.get_image("diff"));
    VAR(mask,           out.get_image("mask"));
    VAR(defect_present, out["defect_present"].as_bool(false));
    VAR(score,          out["score"].as_double(0.0));
    VAR(largest_area,   out["largest_area"].as_int(0));
    VAR(kept_regions,   out["kept"].as_int(0));
    VAR(total_regions,  out["total_regions"].as_int(0));
    VAR(bbox_x0,        out["bbox_x0"].as_int(-1));
    VAR(bbox_y0,        out["bbox_y0"].as_int(-1));
    VAR(bbox_x1,        out["bbox_x1"].as_int(-1));
    VAR(bbox_y1,        out["bbox_y1"].as_int(-1));
}
