#pragma once
//
// xi_var.hpp — VAR()/EMIT() interface (value-tracking functionality REMOVED).
//
// The per-run value store (ValueStore/VarTraits) and the `vars` + JPEG-preview
// wire output were removed from the core in 2026-06 (branch
// refactor/remove-var-core). Script-side data is surfaced via a preview plugin
// instead — the core no longer owns "how a script's output is collected and
// shipped."
//
// These macros are kept as COMPILE-ONLY STUBS so the ~85 existing scripts that
// call VAR()/EMIT() still build unchanged. VAR(name, expr) still evaluates
// `expr` exactly once and leaves an in-scope `name` for later C++ — it simply no
// longer publishes anything. EMIT(name) is now a no-op.
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
