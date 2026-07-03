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
#include <xi/xi_script_pack.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::ScriptPackBuilder cb;
    cb.add_i64("frame", frame);
    auto out = xi::use("healer").process(cb.seal());
    // Surface the healer's count + healed marker through `expose` (channel "qa").
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "qa");
    b.add_i64("count", out.get_i64("count").value_or(-1));
    b.add_bool("healed", out.get_bool("healed").value_or(false));
    xi::use("expose").push(b.seal());
}
