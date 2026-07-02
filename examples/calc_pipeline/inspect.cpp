// calc_pipeline — a two-plugin DATA pipeline (no image processing).
//
//   input ──▶ adder0 ( value + addend ) ──▶ multiplier0 ( value × factor ) ──▶ answer
//
// Each run pushes the frame counter through both plugins and publishes the
// intermediate + final numbers through the `expose` plugin — the current
// data-out surface (the old VAR / Variable-Window path was removed in v9).
// A UI tabs between `expose` channels; here everything goes on channel "calc".
//
// addend / factor come from each instance's config (instances/<name>/instance.json),
// so you can retune them live in the instance UI without editing this script.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& adder = xi::use("adder0");        // value + addend
    auto& mul   = xi::use("multiplier0");   // value × factor

    double input = frame;                   // the number we feed in this run

    // Stage 1 — add. The plugin reads in["value"], returns { result, ... }.
    auto   added   = adder.process(xi::Record().set("value", input));
    double sum     = added["result"].as_double();

    // Stage 2 — multiply stage-1's result.
    auto   product = mul.process(xi::Record().set("value", sum));
    double answer  = product["result"].as_double();   // (input + addend) × factor

    // Surface the intermediate + final values. Build a plain xi::Record, tag it
    // with a "$channel", and push it to the `expose` plugin.
    xi::use("expose").process(xi::Record()
        .set("$channel", "calc")
        .set("input",  input)
        .set("added",  sum)
        .set("answer", answer));
}
