#pragma once
//
// xi_quiesce_token.hpp — compile-enforced "dispatch is quiesced" capability.
//
// Every DESTRUCTIVE PluginManager method — one that mutates or frees adapters,
// instances, or plugin DLLs that dispatch workers could otherwise still be
// calling into — takes a `const xi::QuiesceToken&` as its FIRST parameter:
// proof the caller has quiesced dispatch (stopped + joined the pool, drained
// in-flight runs, taken run_mu). The token cannot be constructed by ordinary
// code, so FORGETTING the quiesce is a compile error instead of a review
// finding. This retires the recurring "lifecycle op X forgot the quiesce
// ritual" use-after-free family (RT5 / J2 / J3 / G2 / L1 / O2 / N1 — ~9
// historical defects across ~16 call sites): the per-call-site convention is
// now a type. Same structural move as InflightRuns::launch() (the missed
// bump-then-check that *was* a UAF became one primitive).
//
// Exactly two minters:
//
//   1. DispatchPoolGuard (backend/src/service_internal.hpp) — the RAII guard
//      returned by quiesce_dispatch_for_lifecycle_op_(). It performs the real
//      quiesce and holds a token; pass `guard.token()`. This is THE way to
//      call a destructive method from anything that can run while dispatch is
//      live (every WS command handler).
//
//   2. QuiesceToken::assert_no_dispatch() — an explicit, greppable assertion
//      by the caller that NO dispatch pool exists or can launch: boot-time
//      plugin scans before the serve loop starts, controlled shutdown
//      teardown AFTER its own stop+drain, the headless runner, and unit
//      tests that drive a bare PluginManager. NEVER call this from a WS
//      command handler — use quiesce_dispatch_for_lifecycle_op_ there.
//
// The token is deliberately non-copyable: you either hold the guard (whose
// token dies with the quiesce window) or you made the explicit no-dispatch
// assertion at the call site.
//

struct DispatchPoolGuard;   // service-side minter (backend/src/service_internal.hpp)

namespace xi {

class QuiesceToken {
public:
    QuiesceToken(const QuiesceToken&) = delete;
    QuiesceToken& operator=(const QuiesceToken&) = delete;

    // Minter #2 (see header comment): caller ASSERTS no dispatch pool exists
    // or can launch (boot / post-drain teardown / headless runner / tests).
    // C++17 guaranteed copy elision constructs the return object in place, so
    // the deleted copy/move never fires.
    static QuiesceToken assert_no_dispatch() { return QuiesceToken{}; }

private:
    QuiesceToken() = default;
    friend struct ::DispatchPoolGuard;   // minter #1: the real quiesce guard
};

} // namespace xi
