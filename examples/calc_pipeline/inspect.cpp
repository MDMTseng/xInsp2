// calc_pipeline — a two-plugin DATA pipeline (no image processing).
//
//   input ──▶ adder0 ( value + addend ) ──▶ multiplier0 ( value × factor ) ──▶ answer
//
// Each run pushes the frame counter through both plugins and publishes the
// intermediate + final numbers to the Variable Window. The two plugins are
// plain C ABI DLLs that move xi::Record JSON only — no xi::Image anywhere.
//
// addend / factor come from each instance's config (instances/<name>/instance.json),
// so you can retune them live in the instance UI without editing this script.

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& adder = xi::use("adder0");        // value + addend
    auto& mul   = xi::use("multiplier0");   // value × factor

    // `VAR(name, expr)` declares `name`, publishes it to the Variable Window,
    // and yields the value — so each stage's tracked variable feeds the next.
    // (Pick names NOT already used as locals; `VAR(x, x)` would redefine `x`.)
    VAR(input, (double)frame);              // the number we feed in this run

    // Stage 1 — add. The plugin reads in["value"], returns { result, ... }.
    VAR(added, adder.process(xi::Record().set("value", input)));

    // Stage 2 — multiply stage-1's result.
    VAR(product, mul.process(xi::Record().set("value", added["result"].as_double())));

    // Final answer: (input + addend) × factor.
    VAR(answer, product["result"].as_double());
}
