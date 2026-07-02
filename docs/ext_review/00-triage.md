# External Review — Triage & Decision Record

| Field       | Value                                                             |
|-------------|-------------------------------------------------------------------|
| **Date**    | 2026-07-02                                                        |
| **Status**  | Maintainer triage (decisions recorded; execution tracked below)   |
| **Scope**   | The 5 advisory reviews in this folder (01 taste · 02 core+dev-ux · 03 traceability · 04 recipe/config integrity · 05 perf/determinism) |
| **Owner**   | Core maintainer                                                   |

This document sits alongside the five advisory reviews and records, for each
notable finding, whether it will be **fixed now**, **scheduled**, **kept out of
core on purpose**, or **deferred as premature**. It is a decision record, not a
work log — where a finding is deferred, the reasoning is captured so we don't
re-litigate it next quarter.

## The lens

Every finding is measured against the project spine and the project's stage —
nothing else:

- **Speed-first.** The per-frame path is a hot path. Nothing that adds I/O,
  allocation, or synchronization to that path survives without a measurement.
- **Minimal core.** The core does only what nothing else can: dispatch,
  lifecycle, crash-safety, refcounted pools, the frozen ABI. It holds zero
  plugin-specific knowledge.
- **Functionality-as-plugins.** New capability is a plugin (or plugin
  composition), not a core feature. The governing test: **"Can this be a
  plugin? If yes, it must be."**
- **ABI v11 is frozen.** New host capability ships as a carved
  `get_interface(id, version)` interface, never as a new struct field. A layout
  break is a deliberate, versioned, once-in-a-major event.
- **No external users yet.** xInsp2 is pre-1.0 and first-party only. This is
  decisive for a whole class of findings: governance / compliance / audit /
  approval machinery has **no consumer yet**. Building it now would mean
  carrying representations we must keep in sync against a user need that does
  not exist. We build it when a user hits the wall, not before.

The buckets below apply this lens. A finding landing in C or D is **not** a
rejection of its merit — it is a statement about *where* it belongs (a plugin)
or *when* it belongs (post-1.0, on real demand).

## Bucket A — Fix now (cheap truth / label / template / doc corrections)

Low-risk, high-integrity fixes: the code or docs currently say something that
isn't true, and correcting it costs almost nothing. These land on master
immediately (see "Two branch sets" below — all of these are non-breaking).

- **Generated sample teaches retired API** (02 P0, *confirmed live*). The
  scaffolded sample project still teaches the retired `VAR` / `xi_inspect_entry`
  surface. The `VAR` macros no longer exist, so a freshly generated project
  likely won't even compile. Update the generator's template to the current API.
- **`xi.hpp` OpenCV comment.** The header comment overstates/misstates the
  OpenCV coupling relative to the current build. Correct the comment to match
  reality.
- **TriggerBus / ABI trigger-correlation comments.** The comments describing
  trigger correlation drifted from the code. Bring them back in line.
- **Perf truth-in-labeling** (05 P0). Fix names/labels that promise more than
  the code delivers:
  - `inspect`-compute naming — call the compute-only span what it is.
  - HMI throughput = **completed** rate (not offered/attempted rate).
  - dispatch "group" = **worker-capacity isolation**, not a scheduling policy.
  - "no-alloc" wording — state it as *no-alloc on the measured path*, not a
    global guarantee.
  - timer is **soft**, not a hard real-time deadline.
  - perf gates report a **best-case** number; label them as such.
- **Additive `run_result` identity** (03 P0). Add identity/context fields to the
  result record without changing existing ones: `trigger_id`, `boot_id`,
  `inspection_id`, `schema` (version), and a `reason_code` / `class`. Purely
  additive — safe on master.
- **Perf baseline env fingerprint + JPEG backend key** (05 P1). Record the
  environment fingerprint (CPU, build flags, accelerators) and the active JPEG
  backend alongside baseline numbers so a baseline is interpretable later.
- **Version policy decision** (01 #6). Declare the independent-versioning model
  with a known-compatible matrix (done in `README.md` → *Versioning &
  compatibility*).

## Bucket B — Core-worthy but deliberate (schedule, don't rush)

These belong in core, but they touch wire formats, enforcement, or hot-path
benchmarks. They are scheduled for a coordinated cutover, not slipped in ad hoc.

- **Full identity slice** (03). Extend the additive identity work into the
  complete slice: station id + boot id + inspection id **and** the script
  generation carried in the result. The additive subset lands in Bucket A;
  finishing the slice (and anything that changes shape) is scheduled here.
- **Runner verdict wiring** (03 #14) — DONE. The result callback is wired in
  `xinsp-runner`: each frame records `code` / `class`
  (ok / ng / na / no_verdict / crashed) / `msg`, and the summary carries a
  `counts` tally. The execution/crash log is now also a pass/fail log. The
  process exit code still reflects infra/crash status only, not the verdict —
  README and the runner's header comment describe the implemented behaviour.
- **Read-only input enforcement** (02 I.4). Enforce that inspection input images
  are read-only, via a newly carved interface splitting `image_read` /
  `image_write`. A carved interface, so ABI-clean — but a real surface, so
  scheduled.
- **Partial-activation honesty** (04). `commit_group` / `load_project` should
  report partial success honestly instead of an all-or-nothing boolean. Changes
  a return contract → scheduled for coordinated cutover.
- **Replace stale hot-path benchmark** (05 #6). `bench_record` no longer
  measures the current hot path; replace it with a benchmark that does.

## Bucket C — Never in core (belongs in a plugin — the reviews agree)

These are real capabilities, but they are **plugin-shaped**, not core-shaped.
Notably, the reviews reach this conclusion themselves via their own guardrails:
each of these is something the core must stay ignorant of. Core exposes the
primitives (the additive result identity from Bucket A, the emit/replay path);
a plugin composes the policy.

- **Evidence / integrity plugin territory** (03 #11, #12, #15, #16, #17):
  evidence journal, integrity-signing, retention, access-disposition,
  on-disk replay. All are policy over the result/record stream — a plugin
  subscribes and enforces; the core never learns what "evidence" is.
- **Recipe governance territory** (04 #15, #16, #17, #19, P5): recipe catalog,
  approval, lifecycle, rollback classes, post-activation verify, audit history.
  These are a recipe-management plugin's job over the existing project/instance
  surface.
- **MES delivery + physical-part identity semantics.** Delivering results to an
  MES and reasoning about physical-part identity are integration concerns —
  a plugin owns the transport and the identity mapping, not the core.

## Bucket D — Not now (premature; no pre-1.0 user need; adds sync burden)

Sound ideas whose cost today is carrying more representations to keep in sync
against a need no user has yet. Revisit on real demand, not on principle.

- **RuntimeContext / split `service_main.cpp` into subsystem owners.** This is
  Engine Stage 2 and is already deferred.
- **Header-only → `src/core` split** (02 I.10). A structural refactor with no
  user-visible payoff yet.
- **Typed args for all 54 commands** (02 I.8). Do **not** typed-ify all 54;
  fix only the one real bug — `optimize`'s raw-JSON-search argument handling.
- **Refactor `extension.ts` (2864 lines)** (01 #2). It's tooling, not core, and
  it works; a rewrite is churn without demand.
- **HMI operator state-model redesign + design tokens** (01 #4 / #5). UI polish
  with no operator asking for it pre-1.0.
- **Optimistic concurrency / CAS; dev/commission/prod/service modes;
  approval/audit** (04 #8, #18, #19). Governance machinery with no consumer yet.
- **Concurrency / determinism build-out** (05 P4 / P5): machine-wide concurrency
  budget, bounded `xi::async` executor, critical/observational sink split,
  deadline admission, soak infrastructure, D0–D3 determinism levels, a dedicated
  perf runner. Deferred wholesale — but flag the **two most defensible** if a
  user later hits them:
  - a **bounded `xi::async` executor** (05 #14), and
  - **moving JPEG / viewer encode off the ordered result path** (05 #19).

## Bucket E — The judgment call

One request recurs across four reviews: a **canonical health / state contract
owned by core** (01 #9, 02 I.9, 03, 05 — 4×). It is deferred with the rest, but
it is called out separately because it is different in kind: unlike most of the
deferred items, this one is **genuinely core-shaped rather than plugin-shaped**
— a coherence primitive the core is the natural owner of. Decision: **defer**,
but note that if a coherence pass is ever undertaken, this is the **first** thing
to build. It is the one recurring ask that the plugin test ("can this be a
plugin?") answers with a *no*.

## Two branch sets

Execution splits by breakage, not by bucket:

- **Non-breaking → master immediately.** Truth corrections, additive fields,
  comment/doc fixes, and new tests carry no wire break. All of Bucket A and the
  additive parts of Bucket B land on master as they're ready.
- **Breaking → integration branch, coordinated cutover.** Clean wire renames and
  contract changes are staged on a separate integration branch and cut over
  with the app-dev team in one coordinated step. This set includes:
  - the `inspect_compute_ms` wire rename,
  - flipping caught-crash reporting to `XI_SYS_CRASHED`,
  - the HMI throughput semantics change (completed-rate),
  - partial-status return values (`commit_group` / `load_project`).

  Staging these together avoids a trickle of small breaks and lets consumers
  update once.
