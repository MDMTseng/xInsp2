# Burr audit — round 2 (2026-07-11)

> **Status: EXECUTED.** Four waves landed on `audit/burr-round2`:
> W1 `49fc578`, W2 `be0e14a`, W3 `a8e5955`, W4 = this commit. The audit swept
> the whole backend for "burrs" — sharp leftover edges from the 2026-07 core
> contraction (TypedPack/Record/cooperative-cancel retirements, A4 explicit
> context) — plus the deferred items recorded below with reasons.

## The four lenses

1. **Lifecycle/quiesce correctness** — rituals that survive as per-call-site
   conventions instead of structures (the core-simplification thesis, applied
   to what the contraction left behind).
2. **Fault boundaries** — plugin-entry and script-entry crash containment:
   one boundary, symmetric bookkeeping, no path that skips the gates.
3. **Hot-path mechanics** — per-frame costs that survived the contraction
   (lock scope, string churn, redundant registry lookups).
4. **Residue** — stale claims (comments/help asserting retired behavior as
   current), dead code the retirements orphaned, and near-identical clones
   that had already started to drift.

## What each wave fixed

- **W1 `49fc578`** — lifecycle/quiesce/evict rituals converted to structures
  (lens 1): the remaining forget-the-ritual sites now go through the same
  compile-enforced primitives the QuiesceToken round introduced.
- **W2 `be0e14a`** — one plugin-entry fault boundary (lens 2); command-layer
  dedups; A4 trigger-id symmetry between entry shapes.
- **W3 `a8e5955`** — hot-path mechanical fixes (lens 3); K1 shared-read
  PackRegistry (the global-mutex-per-key-read finding from the
  core-simplification accounting).
- **W4 (this commit)** — residue sweep (lens 4):
  - *Stale claims*: watchdog help/comments rewritten to the one-phase truth
    (per-inspect budget; wedged frame → grace → exit for FE respawn — no
    global cooperative-cancel phase, no thread kills); TypedPack/PackSchema
    references rewritten to the surviving reality (the dynamic Pack container;
    `ScriptTypedPack` needs only a `Schema::keys` array); dead cross-refs and
    wrong "defined in service_main.cpp" pointers fixed; the loader contract
    comment now states the real entry-symbol rule (either of
    `xi_inspect_entry` / `xi_inspect_entry_tv` suffices); the
    `emit_run_result` crash-path doc now matches the `XI_SYS_CRASHED` caller.
  - *Dead code*: `pack_detail::Slot` (TypedPack's row type); the
    `xi_types_cv.hpp` stub file; the `set_trigger_meta_callback` loader slot
    (the export no longer exists); the `use_grab_cb` stubs (hosts now pass
    nullptr into the retained grab_fn ABI slot); orphan test extern
    definitions; the `instance_degraded`/`mark_instance_degraded`
    back-compat wrappers (callers migrated to `instance_fault`/
    `mark_instance_fault`); `xi_io.hpp` slimmed to its two surviving per-run
    accessors.
  - *Dedups*: the four backend-side JSON-escape clones (one had drifted) now
    route through `xi_json_escape.hpp`; the runner's hand-mirrored result
    band/class/rejection copy replaced by the shared
    `backend/src/xi_result_class.hpp`; one `trigger_id_hex128` formatter;
    one `warn_once_` helper behind the three `warn_use_*` surfaces; the
    duplicated `now_us`/`steady_now_us` compat blocks deleted (all callers
    migrated to the canonical `xi::wall_us`/`xi::mono_us`).

## Deferred — with reasons

- **Report-envelope headers** (`xi_mp_build.hpp`/`xi_pack_report.hpp`,
  762 LOC, 0 consumers) — NOT dead code: a deliberate, recent SDK convention
  awaiting adoption. Keep/delete is CT's call, not an audit cleanup.
- **K2 `$prov` lazy materialization** — touches the wire-shape contract;
  needs its own compatibility decision.
- **B2 lane/group epoch cache** — interacts with the lane-ABA discipline;
  not a mechanical fix.
- **B5 `to_json_into` + `run_started` opt-in** — wire contract change.
- **v1 ambient-trigger path retirement** — a *ratchet*, not a cut: ~30
  example scripts + ~20 extension tests still author the v1 entry. Migrate
  them first, then enforce.
- **TriggerSnapshot + ScriptTypedPack** — public SDK surface; tie any
  retirement to the v1 ratchet above. Note the `check_retired_terms`
  validator currently *pins ScriptTypedPack's existence* — retiring it means
  updating the gate in the same change.
- **PluginManager::project() locked-snapshot split; InstanceRegistration
  pairing guard; public `_locked_` methods → private** — real structural
  work; belongs to the next structural round, not a residue sweep.
- **CapMetrics string-key map + double CapRegistry lookup** — heavy plane,
  low urgency (not on the per-frame path).
- **CLI-args ×3 + sink-holder ×4 consolidation** — cosmetic; batch with the
  next touch of those files.
