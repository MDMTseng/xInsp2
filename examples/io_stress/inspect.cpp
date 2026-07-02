//
// io_stress — exercises the extractor/constructor + nominal-type wiring against
// a RICH fake contract (nested records, a typed array, a custom Feature type).
//
// The WHOLE output is record-class (nominal types carrying their src), so the
// constructor for the next stage is fed extracted typed values directly —
// not plain scalars — and records where each field came from.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_types.hpp>

#include "plugins/synthetic_features/io.hpp"   // synth_io::build / extract / Feature

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    auto syn = xi::use("syn");

    // Stage 1 — seed from literals (no upstream source yet).
    auto e = synth_io::extract(syn.process(synth_io::build().seed(7).threshold(0.30).build()));

    // The custom nominal type Feature: pose / score / edge over one array item.
    synth_io::Feature f0 = e.feature(0);            // extractor piped src onto the feature
    std::vector<synth_io::Feature> feats = e.features();

    // Stage 2 — the input is built ENTIRELY from EXTRACTED record-class values.
    // Each typed value carries its src ("syn"), so the constructor records the
    // provenance of every field. No plain scalars threaded by hand.
    auto in2 = synth_io::build()
                   .seed(e.count())          // xi::Number
                   .threshold(e.mean_score()) // xi::Number
                   .roi(e.roi())             // xi::Roi
                   .build();

    auto e2 = synth_io::extract(syn.process(in2));

    // NA: no seed → require → NA, and the extractors stay total.
    auto na_out = syn.process(xi::Record());
    auto ne = synth_io::extract(na_out);

    // Surface the extracted typed values through the `expose` plugin (channel
    // "io"). Extractors return nominal types (xi::Number / Point / Pose / Roi /
    // Feature); their scalar accessors feed .set() directly. Field order below
    // is the display order.
    xi::use("expose").process(xi::Record()
        .set("$channel",      "io")
        .set("count",         e.count().value())
        .set("feat_count",    e.feature_count())
        .set("centroid_x",    e.centroid().x())
        .set("best_x",        e.best_pose().x())
        .set("best_score",    e.best_score().value())
        .set("roi_w",         e.roi().w())
        .set("mean_score",    e.mean_score().value())
        .set("f0_score",      f0.score())
        .set("f0_pose_x",     f0.pose().x())
        .set("f0_edge_x2",    f0.edge().x2())
        .set("f0_src",        f0.src())
        .set("vec_size",      (int)feats.size())
        .set("in2_seed_prov", in2.prov_of("seed"))       // "syn"
        .set("in2_thr_prov",  in2.prov_of("threshold"))  // "syn"
        .set("in2_roi_prov",  in2.prov_of("roi"))        // "syn"
        .set("out2_count",    e2.count().value())
        .set("out2_roi_w",    e2.roi().w())              // fed ROI round-trips back (640)
        .set("na_is_na",      na_out.is_na())
        .set("na_reason",     na_out.na_reason())
        .set("na_count",      ne.count().value())        // 0
        .set("na_feature_na", ne.feature(0).is_na()));
}
