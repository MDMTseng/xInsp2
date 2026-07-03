#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
XI_SCRIPT_EXPORT void xi_inspect_entry(int){
    xi::ScriptPackBuilder b;
    b.add_str("$channel", "qa");
    b.add_bool("ok", true);
    xi::use("expose").push(b.seal());
}
