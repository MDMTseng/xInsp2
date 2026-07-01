// script_selfcheck.cpp — compile-only guard for SCRIPT self-sufficiency.
//
// xi_script_support.hpp is FORCE-INCLUDED (cl.exe /FI) into every inspection
// script, so it must be fully self-contained: everything it names must resolve
// from its own #includes, NOT from a transitive include that a refactor can
// sever. This exact regression shipped once — dropping xi_instance.hpp from the
// xi.hpp umbrella left xi_script_support.hpp unable to see
// xi::InstanceRegistry::instance(), breaking every script compile — and the
// backend's own TUs did NOT catch it (they pull the header in from elsewhere).
//
// This TU includes ONLY xi_script_support.hpp (the way a bare script sees it).
// If the header ever stops being self-sufficient, THIS translation unit fails to
// compile, and the `script_selfcheck` ctest (which builds this target) goes red.
// Compile-only (OBJECT library) — no link, so it guards header self-sufficiency
// without dragging in link-time symbol resolution.
#include <xi/xi_script_support.hpp>

// A trivial entry so the TU is non-empty; the value is purely that it COMPILED.
int xi_script_selfcheck_ok() { return 0; }
