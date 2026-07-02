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
//   plus diagnostics (total / rejected / cleaned mask) for live tuning,
//   all pushed to the `expose` plugin.
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

    auto& det    = xi::use("det");
    auto& bucket = xi::use("bucket");

    // 1. Detector: gradient-tolerant dark-on-bright binary mask.
    auto det_out = det.process(xi::Record().image("src", frame));

    // 2. Bucket counter: close + label + area-filter + classify.
    auto bk = bucket.process(xi::Record().image("mask", det_out.get_image("mask")));

    // Surface inputs, per-bucket counts + diagnostics through the `expose` plugin.
    xi::use("expose").process(xi::Record()
        .set("$channel", "buckets")
        .image("input",         frame)
        .set("frame_path",      path)
        .image("mask",          det_out.get_image("mask"))
        .set("mask_mean",       det_out["mask_mean"].as_double(0.0))
        .image("cleaned",       bk.get_image("cleaned"))
        .set("count_small",     bk["count_small"].as_int(0))
        .set("count_medium",    bk["count_medium"].as_int(0))
        .set("count_large",     bk["count_large"].as_int(0))
        .set("total_regions",   bk["total_regions"].as_int(0))
        .set("rejected_small",  bk["rejected_small"].as_int(0))
        .set("rejected_big",    bk["rejected_big"].as_int(0)));
}
