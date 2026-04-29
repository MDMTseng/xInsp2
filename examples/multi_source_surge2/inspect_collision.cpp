// inspect_collision.cpp - intentionally trip the VAR(name, name) shadow
// gotcha that PR #22 documented. Compile this on purpose to verify
// (a) cl.exe still emits C2374 and (b) the PR #22 cross-reference in
// xi_var.hpp shows up to a developer who searches the build log.
//
// DO NOT load this as the project's inspect; it's only invoked by
// driver.py for the compile-failure regression sub-step.

#include <xi/xi.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    int foo = 42;
    // This is the natural pattern an unsuspecting author would write:
    // "I have a value `foo`, surface it as a VAR with the same name."
    // The macro expands to `auto foo = ::xi::ValueStore...track("foo", foo)`,
    // collides with the local `int foo`, and cl.exe fires C2374.
    VAR(foo, foo);
}
