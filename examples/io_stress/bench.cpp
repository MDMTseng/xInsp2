//
// bench.cpp — time the extract/construct wiring overhead.
//
// Same data, two ways, 100 iterations each:
//   (A) TYPED  : synth_io::extract(out) + nominal-type accessors + build()
//   (B) RAW    : bare xi::Record path reads + a hand-built input Record
// The delta is what the nominal-type wrappers (shared-Record handles, get_record
// deep copies, set_src/set_prov) cost over touching the cJSON directly.
//
// Compile with optimize:true (/O2) or the numbers are meaningless.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_types.hpp>

#include "plugins/synthetic_features/io.hpp"

#include <chrono>

static inline double now_us() {
    return std::chrono::duration<double, std::micro>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    auto syn = xi::use("syn");
    // One representative output (threshold 0 → all ~7 features kept) to extract
    // from repeatedly. process() itself is NOT in the timed loops.
    auto out = syn.process(synth_io::build().seed(7).threshold(0.0).build());
    auto features_in_output = out.get_array_size("features");

    const int N = 100;
    volatile double sink = 0;

    // ---- (A) TYPED: extract + construct ------------------------------------
    for (int w = 0; w < 10; ++w) { auto e = synth_io::extract(out); sink += e.count().value(); } // warm up
    double a0 = now_us();
    for (int i = 0; i < N; ++i) {
        auto   e  = synth_io::extract(out);
        double s  = e.centroid().x() + e.roi().w() + e.best_pose().angle() + e.mean_score().value();
        for (auto& f : e.features()) s += f.score() + f.pose().x() + f.edge().x2();
        auto in = synth_io::build().seed(e.count()).threshold(e.mean_score()).roi(e.roi()).build();
        sink += s + (double)in.prov_of("roi").size();
    }
    double typed_us = now_us() - a0;

    // ---- (B) RAW: bare Record access ---------------------------------------
    for (int w = 0; w < 10; ++w) { sink += out["count"].as_int(); }
    double b0 = now_us();
    for (int i = 0; i < N; ++i) {
        double s = out["centroid.x"].as_double() + out["roi.w"].as_double()
                 + out["best.pose.angle"].as_double() + out["summary.mean_score"].as_double();
        const int fc = out.get_array_size("features");
        for (int j = 0; j < fc; ++j) {
            auto f = out.get_array_item("features", j);
            s += f["score"].as_double() + f["pose.x"].as_double() + f["edge.x2"].as_double();
        }
        xi::Record in;
        in.set("seed", out["count"].as_int())
          .set("threshold", out["summary.mean_score"].as_double())
          .set("roi", out.get_record("roi"));
        sink += s + (double)in.data_json().size();
    }
    double raw_us = now_us() - b0;

    // Publish the timings through the `expose` plugin (channel "bench").
    xi::use("expose").process(xi::Record()
        .set("$channel",           "bench")
        .set("features_in_output", features_in_output)
        .set("typed_total_us",     typed_us)
        .set("typed_per_iter_us",  typed_us / N)
        .set("raw_total_us",       raw_us)
        .set("raw_per_iter_us",    raw_us / N)
        .set("overhead_ratio",     raw_us > 0 ? typed_us / raw_us : 0.0)
        .set("checksum",           (double)sink));   // keep the loops alive
}
