# Core Fix Plan — open residue

**Status (2026-07-01):** the Parts I–IV plan is **implemented**. The full plan — the
evergreen analysis *and* the landed remediation — is archived verbatim at
[`../archive/core_fix_plan-2026-07.md`](../archive/core_fix_plan-2026-07.md). This file
tracks only what is **still open**.

> **`§`-number citations resolve to the archive.** Every `core_fix_plan.md §N` reference
> across the source, tests, and docs points at the archived full plan
> (`../archive/core_fix_plan-2026-07.md`), which keeps all `§1`–`§28` anchors intact. No
> reference was rewritten in the split.

**What landed** (see the archive for detail + the git log / tests for proof):
- **Part I** — parallel-region context safety: A1 fail-loud, A2 `trigger_snapshot()`,
  **A4 explicit-trigger entry** (`xi_inspect_entry_tv(const xi_trigger_view*, int)` +
  the `XI_INSPECT_ENTRY(t, frame)` macro — the host passes a self-contained trigger,
  the ambient thread_local seam is gone from the user contract; legacy
  `xi_inspect_entry(int)` still supported), B1 `xi::parallel_for`, B2 load-time OMP
  warmup, C1/C2/C3 owner propagation (`test_parallel_safety`, `xi_parallel.hpp`,
  owner thunks).
- **Part II** — core minimization + host-API evolution: Phases 0–4, i.e. freeze guard +
  golden plugin → `get_interface` door → `xi.preview` carve → per-domain interfaces →
  **retire the monolith (ABI v11 大破大立)**.
- **Part III** — plugin-management hardening: G1 subprocess certify, G2 per-plugin
  culprit stamp + quarantine, G3.1 lifecycle×thread contract docs.
- **Part IV** — tooling: T0.1 sanitizers (ASan live), T0.2 salvaged fuzz smoke, T0.3 perf
  gates, T1.1/T1.2 clang-cl in-process libFuzzer + UBSan, and the §28 race nets
  (`test_emit_gate`, `test_hot_reload_swap`, `test_owner_cancel_stress`).

---

## Still open

| Item | From | State | Flips / unblocks when |
|---|---|---|---|
| **G3.2 — debug illegal-transition asserts** in `CAbiInstanceAdapter` (e.g. `process` before `commit`, `set_def` during `process` on a non-reentrant instance). | archive §18 G3.2 | Deferred (the contract is documented in G3.1; the asserts that *enforce* it are the follow-up). | Want the lifecycle×thread contract machine-checked, not just written. |
| **G4 — `code_change`-style state-migration hook** instead of drop-on-mismatch. | archive §18 G4 | Deferred (`OQ-5`). | Cross-version state continuity becomes a stated requirement. |
| **§28 action 2 — Linux CI lane → real TSan** (also unlocks UBSan/libFuzzer at scale). | archive §28 | **Blocked on `OQ-1`.** Windows has no reliable TSan; the race nets currently lean on ASan + `XINSP2_STRESS_SCALE`. | User stands up a Linux CI lane. |
| **OQ-7 partial gaps** — observability export (metrics/latency histograms), WS protocol *negotiation*, Record cross-plugin *static* schema contract. | archive §21, §27.5 | Backlogged; "don't gold-plate" (archive Invariant §27.5). | A concrete need appears (e.g. a production-line monitoring requirement, a client-compat break). |

### Deliberate non-gaps (recorded so they aren't re-chased)
- **A4 in-repo example migration** — the SDK entry, host wiring, loader, and compat
  fallback all landed and are proven (`test_parallel_safety` A4 cases + a real
  force-include compile of a migrated script exporting `xi_inspect_entry_tv`). The
  ~90 `examples/*/inspect.cpp` scripts were **intentionally left on the legacy
  `xi_inspect_entry(int)` path** (one, `parallel_inspect_demo`, migrated as the
  reference) — legacy is supported forever, so bulk migration is churn, not a fix.
  New scripts should use `XI_INSPECT_ENTRY(t, frame)`.
- **GroupLane deque / drop-policy** race — covered black-box by the r8 surge tests; the
  EmitGate primitive it feeds now has a dedicated probe (archive §28 Tier-1).
- **status coalesce map** — a `static` mutex-guarded `std::map` in `service_main.cpp`
  (`set_status_internal`); already the solved lock pattern, not header-isolatable, covered
  black-box by the `qa_*` WS tests (archive §28 Tier-3). Not gold-plated with an exe-linked harness.

---

Decisions and their trail live in
[`OPEN-QUESTIONS.md`](OPEN-QUESTIONS.md); the frozen-ABI rule in
[`adr-001-host-api-freeze.md`](adr-001-host-api-freeze.md).
