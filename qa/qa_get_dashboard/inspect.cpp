#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
// Pack-only heartbeat: expose is a sink, so a script-built pack is pushed
// (the pack equivalent of the retired process(Record) leg). The driver only
// exercises the get_dashboard protocol call — this leg just proves the script
// compiles and surfaces on the pack plane.
XI_SCRIPT_EXPORT void xi_inspect_entry(int){
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "qa");
    b.add_bool("ok", true);
    xi::use("expose").push(b.seal());
}
