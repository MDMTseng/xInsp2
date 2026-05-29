//
// inspect.cpp — qa_fault_heal, single-stage script (FE-E5 fixture).
//
// Plumbs one call into the "healer" instance. On backend start #1 the first
// frame hard-crashes the backend (marker absent). After the FE respawns, the
// marker is present so every frame returns a count and the backend stays
// healthy — letting the FE confirm a healthy port and CLEAR SAFE STATE once.
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& healer = xi::use("healer");
    auto out = healer.process(xi::Record().set("frame", frame));
    VAR(count, out["count"].as_int(-1));
    VAR(healed, out["healed"].as_bool(false));
}
