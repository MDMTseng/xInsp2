//
// graph_demo — a 3-stage pipeline you can SEE in the Pipeline Graph.
//
// Three instances of blob_centroid_detector chained by their "cleaned" image:
//
//     a ──cleaned──▶ b ──cleaned──▶ c
//
// Open the Pipeline Graph (the type-hierarchy icon in the Instances view title,
// or the "xInsp2: Open Pipeline Graph" command), then click "⟳ Capture dataflow":
// the backend records one run and draws the arrows above — observed IMAGE
// dataflow, labelled with the image key that flows. The VAR chips between the
// nodes are the script's own compute (not a traced edge).
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    xi::Image frame = xi::imread(xi::current_frame_path());
    if (frame.empty()) { VAR(error, std::string("no frame — run with a frame_path")); return; }
    VAR(input, frame);

    auto a = xi::use("a");
    auto b = xi::use("b");
    auto c = xi::use("c");

    // Stage 1: detect on the raw frame.
    auto a_out = a.process(xi::Record().image("src", frame));
    VAR(a_count, a_out["count"].as_int(0));

    // Stage 2: feed a's cleaned mask back in as the next stage's source.
    auto b_out = b.process(xi::Record().image("src", a_out.get_image("cleaned")));
    VAR(b_count, b_out["count"].as_int(0));

    // Stage 3: and again — three connected nodes.
    auto c_out = c.process(xi::Record().image("src", b_out.get_image("cleaned")));
    VAR(c_count, c_out["count"].as_int(0));
}
