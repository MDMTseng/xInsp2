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
    // A line gate that reads `defect_present==false` as "good part" would PASS
    // every camera drop-out / mis-configuration if we emitted false on failure.
    // So every early (failed) return sets `inspection_valid=false`: a consumer
    // MUST gate on inspection_valid before trusting defect_present. Only the
    // real comparison below sets it true. (Same idea as circle_counting's -1
    // count sentinel — a "this is not a real verdict" signal.)
    auto ref_out = ref.process(xi::Record{});
    bool ref_loaded = ref_out["loaded"].as_bool(false);
    if (!ref_loaded) {
        // inspection_valid=false marks this as "not a real verdict" — a consumer
        // MUST gate on it before trusting defect_present. Pushed on the same
        // `defect` channel so the sentinel rides next to the value it guards.
        xi::use("expose").process(xi::Record()
            .set("$channel", "defect")
            .set("reference_loaded", ref_loaded)
            .set("error",            ref_out["error"].as_string("reference not loaded"))
            .set("inspection_valid", false)
            .set("defect_present",   false)
            .set("score",            0.0));
        return;
    }
    auto ref_img = ref_out.get_image("reference");

    // 2. Load this frame.
    auto src_out = src.process(xi::Record().set("idx", idx));
    if (!src_out["loaded"].as_bool(false)) {
        xi::use("expose").process(xi::Record()
            .set("$channel", "defect")
            .set("reference_loaded", ref_loaded)
            .set("error",            src_out["error"].as_string("frame load failed"))
            .set("inspection_valid", false)
            .set("defect_present",   false)
            .set("score",            0.0));
        return;
    }
    auto frame = src_out.get_image("frame");

    // 3. Compare.
    auto out = finder.process(xi::Record()
        .image("reference", ref_img)
        .image("frame",     frame));

    // Surface the verdict + diagnostic images through the `expose` plugin. A real
    // comparison ran, so inspection_valid=true — defect_present is now trustworthy.
    xi::use("expose").process(xi::Record()
        .set("$channel", "defect")
        .set("reference_loaded", ref_loaded)
        .image("reference",      ref_img)
        .image("input",          frame)
        .set("frame_path",       src_out["path"].as_string(""))
        .set("inspection_valid", true)
        .image("diff",           out.get_image("diff"))
        .image("mask",           out.get_image("mask"))
        .set("defect_present",   out["defect_present"].as_bool(false))
        .set("score",            out["score"].as_double(0.0))
        .set("largest_area",     out["largest_area"].as_int(0))
        .set("kept_regions",     out["kept"].as_int(0))
        .set("total_regions",    out["total_regions"].as_int(0))
        .set("bbox_x0",          out["bbox_x0"].as_int(-1))
        .set("bbox_y0",          out["bbox_y0"].as_int(-1))
        .set("bbox_x1",          out["bbox_x1"].as_int(-1))
        .set("bbox_y1",          out["bbox_y1"].as_int(-1)));
}
