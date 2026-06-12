//
// io_stress — exercises the extractor/constructor + nominal-type wiring against
// a RICH fake contract (nested records, a typed array, a custom Feature type).
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_types.hpp>

#include "plugins/synthetic_features/io.hpp"   // synth_io::build / extract / Feature

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    auto syn = xi::use("syn");

    // Construct a typed input (seed + threshold) and run.
    auto out = syn.process(synth_io::build().seed(7).threshold(0.30).build());
    auto e   = synth_io::extract(out);

    // Scalars + nested + aggregate extractors.
    VAR(count,       e.count());
    VAR(feat_count,  e.feature_count());
    VAR(centroid_x,  e.centroid().x());
    VAR(best_x,      e.best_pose().x());
    VAR(best_score,  e.best_score());
    VAR(roi_w,       e.roi().w());
    VAR(mean_score,  e.mean_score());
    VAR(out_src,     out.src());          // provenance: "syn"

    // The custom nominal type Feature: pose / score / edge over one array item.
    synth_io::Feature f0 = e.feature(0);
    VAR(f0_score,    f0.score());
    VAR(f0_pose_x,   f0.pose().x());
    VAR(f0_edge_x2,  f0.edge().x2());
    VAR(f0_src,      f0.src());            // extractor piped src onto the feature

    // The typed array.
    std::vector<synth_io::Feature> feats = e.features();
    VAR(vec_size,    (int)feats.size());
    VAR(last_score,  feats.empty() ? -1.0 : feats.back().score());

    // NA: no seed → require → NA, and the extractors stay total.
    auto na = syn.process(xi::Record());
    VAR(na_is_na,    na.is_na());
    VAR(na_reason,   na.na_reason());
    auto ne = synth_io::extract(na);
    VAR(na_count,        ne.count());            // 0
    VAR(na_feat_count,   ne.feature_count());    // 0
    VAR(na_feature_na,   ne.feature(0).is_na()); // typed NA propagates
}
