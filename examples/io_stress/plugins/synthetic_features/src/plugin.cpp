//
// synthetic_features.cpp — emits a rich, deterministic FAKE feature set.
//
// Pure data (no OpenCV): from a `seed` it generates N features, each a nested
// {pose:{x,y,angle}, score, edge:{x1,y1,x2,y2}}, plus aggregates (count,
// centroid, best, summary). Its only purpose is to stress the extractor /
// constructor + nominal-type wiring with nested records and typed arrays.
//
// process(input):
//   input "seed"      : number  REQUIRED → missing makes it NA
//   input "threshold" : number  drop features with score < threshold (default 0)
//   input "roi"       : {x,y,w,h} optional, echoed back
//   returns: count, centroid{x,y}, features[], best{pose,score}, roi, summary
//
#include <xi/xi_json.hpp>

#include <algorithm>

class SyntheticFeatures : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        if (auto na = xi::require(input, {"seed"})) return *na;   // validate at the boundary
        const int    seed = input["seed"].as_int(1);
        const double thr  = input["threshold"].as_double(0.0);

        const int n = (seed % 5) + 3;   // 3..7 raw features
        xi::Record out;

        int    accepted = 0;
        double sx = 0, sy = 0, ssum = 0;
        double smin = 1e9, smax = -1e9;
        double bx = 0, by = 0, ba = 0, bscore = -1.0;

        for (int i = 0; i < n; ++i) {
            const double score = ((seed * 7 + i * 13) % 100) / 100.0;
            if (score < thr) continue;
            const double x = (seed * 3 + i * 11) % 640;
            const double y = (seed * 5 + i * 7) % 480;
            const double angle = (i * 30) % 360;

            out.push("features", xi::Record()
                .set("pose", xi::Record().set("x", x).set("y", y).set("angle", angle))
                .set("score", score)
                .set("edge", xi::Record().set("x1", x).set("y1", y).set("x2", x + 20).set("y2", y + 5)));

            ++accepted;
            sx += x; sy += y; ssum += score;
            smin = std::min(smin, score); smax = std::max(smax, score);
            if (score > bscore) { bscore = score; bx = x; by = y; ba = angle; }
        }

        const double cx = accepted ? sx / accepted : 0.0;
        const double cy = accepted ? sy / accepted : 0.0;

        out.set("count", accepted);
        out.set("centroid", xi::Record().set("x", cx).set("y", cy));
        out.set("best", xi::Record()
                            .set("pose", xi::Record().set("x", bx).set("y", by).set("angle", ba))
                            .set("score", bscore < 0 ? 0.0 : bscore));
        if (input.has("roi")) out.set("roi", input.get_record("roi"));
        else                  out.set("roi", xi::Record().set("x", 0).set("y", 0).set("w", 640).set("h", 480));
        out.set("summary", xi::Record()
                               .set("min_score", accepted ? smin : 0.0)
                               .set("max_score", accepted ? smax : 0.0)
                               .set("mean_score", accepted ? ssum / accepted : 0.0));
        return out;
    }
};

XI_PLUGIN_IMPL(SyntheticFeatures)
