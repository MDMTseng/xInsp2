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
        VAR(error, src_out["error"].as_string("frame load failed"));
        VAR(count, int(-1));
        return;
    }
    auto frame = src_out.get_image("frame");
    VAR(input, frame);
    VAR(frame_path, src_out["path"].as_string(""));

    // 2. Detector returns a "mask" image plus diagnostic stats.
    //    (VAR expands to `auto NAME = expr;` so we name the local
    //    differently to avoid the redeclaration trap.)
    auto det_out = det.process(xi::Record().image("src", frame));
    VAR(mask, det_out.get_image("mask"));
    VAR(mask_mean, det_out["mask_mean"].as_double(0.0));

    // 3. Counter cleans the mask, labels regions, applies area filter.
    auto cnt_out = counter.process(xi::Record().image("mask", mask));
    VAR(cleaned,        cnt_out.get_image("cleaned"));
    VAR(total_regions,  cnt_out["total_regions"].as_int(0));
    VAR(rejected_small, cnt_out["rejected_small"].as_int(0));
    VAR(rejected_big,   cnt_out["rejected_big"].as_int(0));
    VAR(count,          cnt_out["count"].as_int(0));
}
