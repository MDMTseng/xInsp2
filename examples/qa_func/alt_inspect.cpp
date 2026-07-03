//
// alt_inspect.cpp — the --script OVERRIDE target for AS-I3.
//
// Identical to inspect.cpp except it stamps script=2. When the backend is
// started with --script=alt_inspect.cpp, AS-I3 attaches a client, runs one
// frame, and asserts the "script" value (surfaced via the `expose` plugin on
// channel "qa") == 2 (i.e. THIS file compiled, not the project.json default
// inspect.cpp which stamps 1).
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Drive the counter instance through its pack door (pack in, pack out).
    xi::ScriptPackBuilder cb;
    cb.add_i64("frame", frame);
    auto out = xi::use("counter").process(cb.seal());
    const long long count = out.get_i64("count").value_or(-1);

    // Surface count + the script marker through `expose` (channel "qa").
    xi::ScriptPackBuilder b;
    b.add_i64("count", count);
    b.add_i64("script", 2);      // override-script marker (default inspect.cpp emits 1)
    b.add_i64("frame", frame);
    b.add_str("$channel", "qa");
    xi::use("expose").push(b.seal());
}
