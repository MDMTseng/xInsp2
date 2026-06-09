//
// multi_file_script — splitting a complex script across files.
//
// A script compiles as ONE translation unit, so the way to split it is to put
// each lane / logical block in its own HEADER and #include it here. Everything
// in those headers — VAR(), xi::use(), xi::status(), state() — works exactly as
// if it were written inline, because it's compiled into this same TU. (You
// CAN'T split into separate .cpp TUs: the script-support thunks + xi::use()
// callback globals are bound per-TU, so a second .cpp can't see them. Headers
// sidestep that entirely.)
//
// Convention: the script-side files share the script's stem — inspect.cpp plus
// inspect_*.hpp siblings. Saving any of them recompiles the whole script (the
// extension associates files by that prefix).
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include "inspect_lane_a.hpp"
#include "inspect_lane_b.hpp"

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    run_lane_a(frame);   // logic lives in inspect_lane_a.hpp
    run_lane_b(frame);   // logic lives in inspect_lane_b.hpp
}
