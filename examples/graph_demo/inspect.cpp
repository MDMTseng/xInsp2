//
// graph_demo — a 3-stage pipeline you can SEE in the Pipeline Graph.
//
// Three instances of blob_centroid_detector chained by their "cleaned" image:
//
//     a ──cleaned──▶ b ──cleaned──▶ c
//
// Two things to try in the Pipeline Graph (type-hierarchy icon in the editor
// toolbar / Instances view title, or "xInsp2: Open Pipeline Graph"):
//
//   1. CLICK A NODE  → opens that instance's webui.
//      The chips between the nodes are the script's own compute — the values it
//      pushes to the `expose` plugin (channel "graph").
//
//   2. Click "⟳ Capture dataflow" → arrows appear for the observed IMAGE
//      dataflow (a ──cleaned──▶ b ──cleaned──▶ c), labelled with the image key.
//
// PROVENANCE: every plugin output is stamped with WHERE it came from — the
// a_src / b_src / c_src fields below read it back (each == its instance name).
// That's the same lineage the graph traces for images, carried on the data
// itself. (The full extractor→constructor→$prov story is in fixturing_demo.)
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    xi::Image frame = xi::imread(xi::current_frame_path());
    if (frame.empty()) {
        xi::use("expose").process(xi::Record()
            .set("$channel", "error")
            .set("error", std::string("no frame — run with a frame_path")));
        return;
    }

    auto a = xi::use("a");
    auto b = xi::use("b");
    auto c = xi::use("c");

    // Stage 1: detect on the raw frame.
    auto a_out = a.process(xi::Record().image("src", frame));

    // Stage 2: feed a's cleaned mask back in as the next stage's source.
    auto b_out = b.process(xi::Record().image("src", a_out.get_image("cleaned")));

    // Stage 3: and again — three connected nodes.
    auto c_out = c.process(xi::Record().image("src", b_out.get_image("cleaned")));

    // Surface the per-stage counts + provenance through the `expose` plugin.
    // *_src reads back the host-stamped origin of each output (== its instance name).
    xi::use("expose").process(xi::Record()
        .set("$channel", "graph")
        .image("input",   frame)
        .set("a_count",   a_out["count"].as_int(0))
        .set("a_src",     a_out.src())     // provenance: "a" — stamped by the host
        .set("b_count",   b_out["count"].as_int(0))
        .set("b_src",     b_out.src())     // "b"
        .set("c_count",   c_out["count"].as_int(0))
        .set("c_src",     c_out.src()));   // "c"
}
