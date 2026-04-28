//
// inspect.cpp — circle_size_buckets, orchestration only.
//
// Pipeline:
//   xi::imread(current_frame_path())
//        ▼
//   "det"     (local_contrast_detector) : (bg − blurred) > C  binary mask
//        ▼
//   "bucket"  (size_bucket_counter)     : close + label + area filter +
//                                         per-bucket classification
//
// Per-frame outputs:
//   count_small / count_medium / count_large  (the deliverable)
//   plus diagnostic VARs (total / rejected / cleaned mask) for live tuning.
//
// Driven by the Python SDK via c.run(frame_path=...). The script reads
// the per-run path through xi::current_frame_path() — no project-local
// PNG-source plugin needed.
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

    auto& det    = xi::use("det");
    auto& bucket = xi::use("bucket");

    // 1. Detector: gradient-tolerant dark-on-bright binary mask.
    auto det_out = det.process(xi::Record().image("src", frame));
    VAR(mask,      det_out.get_image("mask"));
    VAR(mask_mean, det_out["mask_mean"].as_double(0.0));

    // 2. Bucket counter: close + label + area-filter + classify.
    auto bk = bucket.process(xi::Record().image("mask", det_out.get_image("mask")));
    VAR(cleaned,        bk.get_image("cleaned"));
    VAR(count_small,    bk["count_small"].as_int(0));
    VAR(count_medium,   bk["count_medium"].as_int(0));
    VAR(count_large,    bk["count_large"].as_int(0));
    VAR(total_regions,  bk["total_regions"].as_int(0));
    VAR(rejected_small, bk["rejected_small"].as_int(0));
    VAR(rejected_big,   bk["rejected_big"].as_int(0));
}
