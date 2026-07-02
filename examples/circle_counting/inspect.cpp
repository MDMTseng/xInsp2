//
// inspect.cpp — circle counting, orchestration only.
//
// All image math lives in three project plugins:
//   - "src"     (png_frame_source)        : load a PNG by index
//   - "det"     (local_contrast_detector) : (bg − blurred) > C  binary mask
//   - "counter" (region_counter)          : close + label + area filter → count
//
// This file is deliberately small. The user retunes the pipeline by
// opening the instance UIs from the xInsp2 sidebar and dragging
// sliders — no recompile of THIS file required.
//
// The driver loop sets `frame_idx` between runs to walk the test set.
//

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

static xi::Param<int> frame_idx{"frame_idx", 0, {0, 9999}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    int idx = frame_idx;

    auto& src     = xi::use("src");
    auto& det     = xi::use("det");
    auto& counter = xi::use("counter");

    // 1. Source loads <frames_dir>/frame_NN.png and returns it as "frame".
    auto src_out = src.process(xi::Record().set("idx", idx));
    if (!src_out["loaded"].as_bool(false)) {
        xi::use("expose").process(xi::Record()
            .set("$channel", "error")
            .set("error", src_out["error"].as_string("frame load failed"))
            .set("count", int(-1)));
        return;
    }
    auto frame = src_out.get_image("frame");

    // 2. Detector returns a "mask" image plus diagnostic stats. `mask` is a
    //    plain local because the counter stage below consumes it.
    auto det_out = det.process(xi::Record().image("src", frame));
    auto mask = det_out.get_image("mask");

    // 3. Counter cleans the mask, labels regions, applies area filter.
    auto cnt_out = counter.process(xi::Record().image("mask", mask));

    // Surface inputs, diagnostics + the count through the `expose` plugin.
    xi::use("expose").process(xi::Record()
        .set("$channel", "circles")
        .image("input",         frame)
        .set("frame_path",      src_out["path"].as_string(""))
        .image("mask",          mask)
        .set("mask_mean",       det_out["mask_mean"].as_double(0.0))
        .image("cleaned",       cnt_out.get_image("cleaned"))
        .set("total_regions",   cnt_out["total_regions"].as_int(0))
        .set("rejected_small",  cnt_out["rejected_small"].as_int(0))
        .set("rejected_big",    cnt_out["rejected_big"].as_int(0))
        .set("count",           cnt_out["count"].as_int(0)));
}
