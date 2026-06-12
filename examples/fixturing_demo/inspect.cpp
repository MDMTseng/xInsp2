//
// fixturing_demo — the whole typed-I/O story in one script.
//
//   locate (blob_centroid_detector)  →  pose-align + fit a line (line_fit)
//
// The locator's raw cJSON is adapted into typed xi::Pose by its extractor; the
// line_fit input is assembled by its typed constructor; line_fit validates its
// input and returns NA if a pose is missing — and that NA propagates into the
// typed Line with no defensive code in the wiring.
//
// See docs/design/io-types-and-na.md.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_types.hpp>

#include "plugins/blob_centroid_detector/io.hpp"   // blob_io::extract
#include "plugins/line_fit/io.hpp"                 // line_fit_io::build / extract

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    xi::Image frame = xi::imread(xi::current_frame_path());
    if (frame.empty()) { VAR(error, std::string("no frame")); return; }

    auto loc = xi::use("loc");   // locator (blob_centroid_detector)
    auto fit = xi::use("fit");   // line_fit

    // 1) Locate → typed poses. The host stamps the output's $src = "loc".
    auto loc_out = loc.process(xi::Record().image("src", frame));
    VAR(loc_src, loc_out.src());
    auto located = blob_io::extract(loc_out);
    VAR(n_poses, located.orientation_count());

    // 2) Wire pose 0 into line_fit (constructor) and read back the typed Line.
    //    The extractor piped the source onto the pose; the constructor records
    //    where the "current" field came from ($prov), so the data lineage is
    //    self-describing — no script parsing needed.
    xi::Pose pose0 = located.orientation(0);
    VAR(pose0_x,   pose0.x());
    VAR(pose0_y,   pose0.y());
    VAR(pose0_src, pose0.src());

    auto fit_in = line_fit_io::build().current(pose0).build();
    VAR(in_prov_current, fit_in.prov_of("current"));
    auto fit_out = fit.process(fit_in);
    xi::Line line = line_fit_io::extract(fit_out).line();
    VAR(fit_na,  fit_out.is_na());
    VAR(line_x1, line.x1());     // baseline line (10,0)-(10,40) shifted to pose0 (angle 0)
    VAR(line_y2, line.y2());

    // 3) NA propagation: feed line_fit with NO current pose → it returns NA, and
    //    the NA flows straight into the typed Line. No null-checks in between.
    auto na_out  = fit.process(xi::Record());
    xi::Line na_line = line_fit_io::extract(na_out).line();
    VAR(na_out_na,    na_out.is_na());
    VAR(na_reason,    na_out.na_reason());
    VAR(na_line_na,   na_line.is_na());
}
