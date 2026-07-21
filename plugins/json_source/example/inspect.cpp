// json_source example — read the GUI-edited JSON document off the pack plane.
//
// The `src` instance emits ONE sealed pack per tick: a leading `seq` counter
// plus one entry per top-level field of the document you edit in its UI
// (plugins/json_source/README.md has the JSON-value -> pack-entry table).
// Nothing here is json_source-specific plumbing — this is the ordinary way a
// script reads a source's pack.
//
// Try it: open the `src` instance's UI, change `pass_limit` or `part_id`, and
// watch the verdict and the exposed values follow on the next run.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>

XI_INSPECT_ENTRY(t, frame) {
    (void)frame;

    // json_source is a PULL source: its pack door emits when something ticks it,
    // and the emit arrives as a TRIGGER on a later dispatch. So a run is one of
    // two things, and the script handles both:
    //
    //   timer run  (t.is_active() == false) → pump the source once, no verdict
    //   trigger run(t.is_active() == true)  → the document is in t.pack(), judge it
    //
    // The pump run is explicitly NA (result(0)) rather than left unset: "no
    // verdict" would be counted as a defect in the run tally, and it isn't one.
    if (!t.is_active()) {
        xi::ScriptPackBuilder tick;              // the tick input is ignored
        xi::use("src").process(tick.seal());
        xi::result(0, "pumped src — its emit arrives as the next trigger");
        return;
    }

    auto p = t.pack();
    if (!p) { xi::ng(2, "no pack from src"); return; }

    // Scalars land as canonical entries under their JSON field names.
    const long long seq   = p.get_i64("seq").value_or(-1);
    const long long limit = p.get_i64("pass_limit").value_or(-1);
    const double    width = p.get_f64("width_mm").value_or(-1.0);
    const auto      part  = p.get_str("part_id");

    // A `$fault` entry is how json_source reports a document it could not
    // encode (bad root type, depth bomb, oversized nested value) — a contract
    // failure arrives as a normal pack, so check for it rather than guessing
    // from missing keys.
    if (p.get_str("$fault").has_value()) {
        xi::ng(3, "src emitted a $fault pack — check the document in its UI");
        return;
    }
    if (!part || limit < 0) {
        xi::ng(2, "document is missing part_id / pass_limit");
        return;
    }

    // Surface what we read so the values show up in the UI.
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "json");
    b.add_i64("seq", seq);
    b.add_str("part_id", *part);
    b.add_f64("width_mm", width);
    b.add_i64("pass_limit", limit);
    xi::use("expose").push(b.seal());

    // The "inspection": a toy rule over the document's own numbers, so editing
    // the JSON in the UI flips the verdict.
    if (width > 0.0 && width <= (double)limit) xi::ok(1, "within limit");
    else                                       xi::ng(1, "width exceeds pass_limit");
}
