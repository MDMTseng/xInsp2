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

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& counter = xi::use("counter");
    auto out = counter.process(xi::Record().set("frame", frame));
    xi::Record rec;
    rec.set("count", out["count"].as_int(-1))
       .set("script", 2)      // override-script marker (default inspect.cpp emits 1)
       .set("frame", frame);
    rec.set("$channel", "qa");
    xi::use("expose").process(rec);
}
