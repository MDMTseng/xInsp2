#pragma once
//
// xi_var.hpp — VAR()/EMIT() interface (value-tracking functionality REMOVED).
//
// The per-run value store (ValueStore/VarTraits) and the `vars` + JPEG-preview
// wire output were removed from the core in 2026-06 (branch
// refactor/remove-var-core). Script-side data is now surfaced via the `expose`
// plugin — the core no longer owns "how a script's output is collected and
// shipped." MIGRATION (replaces VAR/EMIT):
//
//     xi::Record r;
//     r.set("score", score).image("edges", img);   // values + images, in order
//     r.set("$channel", "lane");                    // channel id (reserved key)
//     xi::use("expose").process(r);                 // generic plugin call, no header
//
// See docs/guides/write-a-script.md ("Surfacing output") and
// docs/roadmap/expose-plugin-and-output-transport.md.
//
// These macros are kept as COMPILE-ONLY STUBS so the ~85 existing scripts that
// call VAR()/EMIT() still build unchanged. VAR(name, expr) still evaluates
// `expr` exactly once and leaves an in-scope `name` for later C++ — it simply no
// longer publishes anything. EMIT(name) is now a no-op. Both are marked
// #pragma deprecated so each call site nudges the author toward `expose`.
//
// (The old machinery — ValueStore, VarEntry, VarKind, VarTraits, the snapshot
// thunk — lives in git history before this branch.)
//

namespace xi {
// No runtime state: VAR/EMIT are pure bindings now.
}  // namespace xi

// VAR(name, expr): evaluate `expr` once, bind `name` for later use in the script.
// Formerly also tracked the value into the per-run store; that store is gone.
#define VAR(name, expr)     auto name = (expr)
#define VAR_RAW(name, expr) auto name = (expr)

// EMIT(name) / EMIT_RAW(name): formerly surfaced an existing in-scope value under
// its own name. Now a void-discard that keeps `name` "used" so scripts that call
// EMIT on a variable still compile warning-clean.
#define EMIT(name)     (void)(name)
#define EMIT_RAW(name) (void)(name)

// Per-use deprecation nudge: emits MSVC C4995 at each VAR/EMIT call site so the
// ~85 legacy scripts get a visible "migrate to xi::use(\"expose\")" signal
// without breaking the build (the macros still expand to the stubs above).
#if defined(_MSC_VER)
#  pragma deprecated(VAR, VAR_RAW, EMIT, EMIT_RAW)
#endif
// TODO(linux): GCC/Clang have no macro-deprecation pragma; surface the notice via
// a one-time -W diagnostic or a [[deprecated]] sentinel when the Linux port lands.
