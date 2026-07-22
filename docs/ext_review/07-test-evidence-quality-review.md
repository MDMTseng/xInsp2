# xInsp2 Test Evidence Quality Review

| Field | Value |
|---|---|
| Date | 2026-07-02 |
| Reviewer | Claude (external advisory) |
| Status | Advisory |
| Scope | Whether the test suite proves what the project claims: release gates, fixture validity, failure injection, crash-isolation coverage, fuzz coverage, protocol-fixture parity, the plugin-SDK testing story, the QA gate, perf-gate validity as *evidence*, hot- vs cold-path coverage, flakiness, and what is deliberately not tested |

Related reviews:

- [`00-triage.md`](./00-triage.md) — maintainer triage (referenced, not re-litigated)
- [`03-production-traceability-review.md`](./03-production-traceability-review.md) — run-outcome / identity contract
- [`04-recipe-configuration-integrity-review.md`](./04-recipe-configuration-integrity-review.md) — commit_group / load_project semantics
- [`05-real-time-performance-determinism-review.md`](./05-real-time-performance-determinism-review.md) — perf-gate methodology (this review does not re-open it)

## Scope

This review asks one question of every test in the tree: **when it is green, what
does that actually prove?** It distinguishes tests that pin a real, implemented
behaviour from tests that pin a hand-written artifact, a removed feature, or a
contract the shipping backend does not yet emit. It also asks what *enforces*
green — a test nobody runs is not a gate.

It covers `backend/tests` (C++ unit + probes), `vscode-extension/test`
(node integration + E2E), `tests/fuzz` and `backend/tests/fuzz` (two fuzz
tiers), `protocol/fixtures`, `sdk/testing` + `sdk/host_mock` (the plugin-author
testing story), and `tools/run_qa.py` / `check_doc_coverage.py` /
`check_retired_terms.py` / `perf_gate.cmake` as gates.

Calibration: xInsp2 is pre-1.0, first-party-only. Heavyweight CI and a formal
soak lab are premature and are not asked for here. The bar applied is narrower:
**a test that is green must not assert something false, stale, or unimplemented.**
A silent no-op gate is a now-problem regardless of stage.

## Executive Summary

The test surface is broad and, in its core, genuinely good. The C++ unit layer
asserts real invariants (refcount integrity, path expressions, the reentrancy
admission gate, SEH/throw non-unwind across the ABI). The failure-injection
tests are a highlight: `plugin_crash_forensics` actually arms a plugin to crash
the process from a raw thread and asserts the minidump + breadcrumb survive, and
`test_set_def_race` / `qa_reentrancy` prove the concurrency model through the
*real* admission gate with concrete max-concurrency assertions. The plugin-SDK
host mock (`xi_run_plugin` over `ImagePool::make_host_api`) lets an author drive
`process()` against nearly the full, real host contract — a level of
testability rare at this stage.

The weaknesses are concentrated and specific, not diffuse:

1. **Nothing enforces green.** There is no CI configuration anywhere in the tree
   (no `.github/`, no pipeline file). Every gate — ctest, the node suites,
   `run_qa.py`, the fuzz smoke — is run by hand. The fuzz smoke was explicitly
   built "to be wired into CI as a build-breaking net" and is not wired to
   anything.
2. **A cluster of protocol fixtures pins a wire shape the shipping backend does
   not emit, and the tests over them assert the fixtures' own literal fields.**
   The `commit_group_*` and `load_project_partial` fixtures describe a
   partial-status contract that (per triage Bucket B) is a *scheduled, breaking,
   not-yet-landed* change — yet the green Python tests present it as implemented
   and covered. This is the single most misleading piece of evidence in the repo.
3. **Fuzz is real but dark by default.** Both fuzz tiers are opt-in and un-gated;
   the two highest-value parser boundaries only ever run if a human remembers.
4. **The canonical testing doc understates the gate surface ~4× and one fixture
   is an orphan for a removed feature** — the inventory a reviewer would use to
   reason about coverage cannot be trusted.

None of these are architectural. They are honesty-of-evidence defects: the suite
is stronger than its worst tests suggest, but its worst tests quietly claim
things that are not true. Fixing them is cheap and mostly means deleting or
re-pointing tests, not writing new subsystems.

## Scorecard

| Dimension | Grade | Rationale |
|---|:--:|---|
| Gate enforcement (CI) | D | Gates exist and are good; nothing runs them automatically — green is a local, manual claim |
| Core unit assertion quality | B+ | Refcount/COW, path exprs, reentrancy gate, ABI non-unwind — real invariants, not smoke |
| Failure injection | A− | Raw-thread crash forensics + throw isolation + reentrancy races are genuine fault tests |
| Crash-isolation coverage | B− | Survival is proven; the crash-*classification* assertion is conditionally skipped |
| Fixture validity / freshness | C− | run_result fixtures are sound; partial/commit_group are ahead of impl; `vars_mixed` is orphaned |
| Protocol fixture parity | C | Only 1 of 11 fixtures is cross-checked C++↔TS; the drift-prone shapes have no parity test |
| Fuzz coverage | C+ | Well-built two-tier harness set, honestly scoped — but entirely opt-in / un-gated; `get_interface` unharnessed |
| Plugin-SDK testing story | B+ | `xi_run_plugin` exercises the real host_api incl. carved interfaces; source/emit + WS sinks not testable headless |
| QA gate (`run_qa.py`) | B | Real sequential, CI-usable aggregator with genuine end-to-end drivers; excludes fuzz; itself manual |
| Perf-gate validity (as evidence) | C | (Defers to review 05.) Baselines now carry a fingerprint, but it is self-reported by the bench process |
| Test-doc / inventory accuracy | C− | README "12 targets" / `testing.md` list vs 44 registered ctests; stale despite an "update on every addition" rule |
| Flakiness risk | B− | Random ports + fixed `sleep`s + acknowledged single-client races; log-timing dependence in `ws_crash` |

## Findings

### 1. No CI: every gate is a manual ritual

There is no continuous-integration configuration in the repository — no
`.github/`, no pipeline YAML, no hook that runs `ctest` / `node --test` /
`run_qa.py` on a change. The gates are real and well-built, but the only thing
that runs them is a human at a keyboard.

The gap is sharpest for fuzz, which was *written* to be enforced.
`tests/fuzz/README.md:10-12` describes the smoke as promoted "from one-shot
`qa/` surveys into a maintained, reduced-iteration smoke that can be wired
into CI as a build-breaking net," and `run_smoke.py:1-13` repeats the intent —
but nothing invokes it. `tools/run_qa.py` runs only `qa/qa_*/driver.py`
and never touches the fuzz smoke; the C++ fuzz targets are gated OFF (finding 5).

#### Consequence

"Green" is a statement about the last time someone ran the suite on their box,
not an enforced invariant. A merge that breaks a node suite or a fuzz harness
lands clean. For a project whose spine is crash-safety and a frozen ABI, the
absence of an automatic "run everything once" net means regressions in exactly
those properties can ship silently between manual runs.

#### Recommendation

A full matrix is premature pre-1.0; a single scripted entrypoint is not. Add one
`run_all` script (ctest + `node --test test/*.test.mjs` + `run_qa.py` +
`run_smoke.py`) and a minimal GitHub Actions (or equivalent) job that runs at
least the socket-free ctest subset and the node suites on push. Even a
non-blocking "trend" job would convert green from a claim into evidence. Wire the
fuzz smoke in at a small budget, exactly as its own README already prescribes.

### 2. The partial-status fixtures pin a contract master does not emit; the tests assert the fixtures' own literals

`protocol/fixtures/commit_group_committed.json`,
`commit_group_partial.json`, and `load_project_partial.json` describe a
lifecycle wire shape with `status` / `committed` / `canonical` / `warnings`
(commit_group) and `ok:false` + `data.status:"partial"` + `error`
(load_project). The shipping backend on `master` emits neither shape:

- `cmd_commit_group_` returns `ok:true` with `{"results":[{name,ok,result},…]}`
  on success and `ok:false, error:"one or more commits failed"` with the same
  `results` array on any failure (`backend/src/service_main.cpp:3489-3496`).
  There is no `status`, `committed`, or `canonical` key anywhere in the handler.
- `load_project`'s partial path returns `send_rsp_ok(srv, id, data)` with
  `{"param_warnings":[…],"instance_warnings":[…]}` — an **`ok:true`** response
  with no `status` field and no `error`
  (`backend/src/service_main.cpp:2902-2917`).

The only tests over these fixtures are
`tools/xinsp2_py/tests/test_run_outcome.py:121-136`, which do
`assert fx["data"]["status"] == "partial"` and construct a `PartialStatusError`
*by hand from the fixture*. That is: the test asserts that a hand-written JSON
file contains the string someone typed into it. It never runs a backend and is
structurally incapable of noticing that master emits a completely different
shape.

Triage Bucket B is explicit that partial-status honesty for
`commit_group`/`load_project` "changes a return contract → scheduled for
coordinated cutover" and is staged on the breaking integration branch, not
master. So the maintainer knows the contract is not landed — but the fixtures
plus green tests read, to anyone scanning coverage, as *implemented and tested*.

#### Consequence

This is the most misleading evidence in the tree. A reader who trusts a green
suite would conclude the client correctly handles partial commits and partial
recipe restores; in fact the client's partial-handling path has never been
exercised against a real backend, and the shape it decodes does not exist on
master. When the breaking cutover lands, these tests will *not* catch a mismatch,
because they are pinned to the aspiration, not to the emitter.

#### Recommendation

Until the breaking contract lands, either (a) move these three fixtures and their
tests onto the integration branch alongside the code that will emit them, or
(b) keep them on master but re-point the tests at the shape master *actually*
emits (`results[]`; `ok:true` + `param_warnings`/`instance_warnings`) and add a
live round-trip that starts a backend, forces a partial commit/load, and asserts
the real wire bytes. A fixture is only evidence if some emitter is contractually
bound to reproduce it.

### 3. An orphaned fixture pins a removed wire message; the repo map claims a consumer that does not exist

`protocol/fixtures/vars_mixed.json` encodes a `{"type":"vars",…}` frame with
per-item `kind`/`gid`. `docs/testing.md:7-16` states plainly that "the `vars`
wire message … [has] been removed from the backend." No test loads the fixture
(a tree-wide search finds it referenced only in prose). Yet
`docs/repository-map.md:59` documents it as a live artifact "used to pin the wire
format … Consumed by protocol tests."

#### Consequence

Dead evidence that reads as live. A fixture for a deleted feature persists in the
canonical fixtures directory, and the repository map asserts a test consumes it —
so the map cannot be trusted as a guide to what is pinned, and a maintainer might
"fix" a drift against a format that no longer exists.

#### Recommendation

Delete `vars_mixed.json` and correct the `repository-map.md` row (the same edit
should drop the `vars` mention). If a record of the retired format is wanted,
move it to `docs/archive/` and label it retired.

### 4. The cross-language protocol parity test covers 1 of 11 fixtures

`protocol/fixtures` holds 11 fixtures. The TypeScript mirror
`vscode-extension/test/protocol.test.mjs` reads exactly one of them
(`cmd_run.json`) and asserts four fields. The C++ side
`backend/tests/test_protocol.cpp:43-49` also parses only `cmd_run.json`. The nine
run-outcome / metrics / lifecycle fixtures have a Python consumer only
(`test_run_outcome.py`).

#### Consequence

`testing.md` advertises `test_protocol` as proving "fixture parity with TS side."
That parity is verified for a single command. The `run_result`,
`run_finished`, and `metrics_snapshot` shapes — the ones the VS Code extension
actually decodes off the wire, and the ones most likely to drift when the backend
emitter changes — have **no** C++↔TS parity check at all. Drift between the C++
emitter and the TS decoder is exactly the failure this suite is supposed to
prevent, and it is unguarded for every event except `cmd`.

#### Recommendation

Extend `protocol.test.mjs` to load and shape-check the run-outcome, run-finished,
and metrics fixtures (the extension already has decoders for them), so both sides
of the boundary assert against the same committed bytes. This is a few dozen
lines and closes the actual drift surface.

### 5. Both fuzz tiers are opt-in and un-gated; the ABI door has no harness

xInsp2 has two well-constructed fuzz tiers, and neither runs unless explicitly
invoked:

- **C++ libFuzzer** (`backend/tests/fuzz/`) targets the highest-value parser
  boundaries — `parse_cmd`, raw `yyjson_read`, and `Record::from_json_bytes`.
  It is gated behind `-DXINSP2_FUZZ=ON` and requires clang-cl; the README is
  explicit that "a normal `cmake` + `ctest` never enters it"
  (`backend/tests/fuzz/README.md:9-13`). The default MSVC build never builds,
  let alone runs, these.
- **Python black-box smoke** (`tests/fuzz/run_smoke.py`) drives the real backend
  over WS. It is standalone and, per finding 1, invoked by nothing.

The harness set is genuinely disciplined — it honestly de-scopes surveys against
removed code (`tests/fuzz/README.md:88-100`, and the §27.3 "a survey against
removed code is not a live finding" rule). But its own notes flag a live gap:
`backend/tests/fuzz/README.md:110-113` records that the ABI v10
`get_interface(id, version)` door "does not exist … yet — no harness was
written." It does now exist and is a shipped, security-relevant surface
(`backend/include/xi/xi_image_pool.hpp:482`, the capability-query door resolving
five carved interfaces).

#### Consequence

The parser/decoder boundaries are where malformed WS input and untrusted record
JSON enter the process, and they are the natural home for the memory-safety
regressions crash-safety is meant to withstand. That net is dark by default. The
newest ABI surface (the interface resolver) is unfuzzed entirely.

#### Recommendation

Wire `run_smoke.py` into the manual `run_all` from finding 1 at a small budget
(its smoke defaults already finish in ~45s). Document a periodic (e.g. nightly or
pre-release) longer libFuzzer sweep even if it stays manual. Add a
`fuzz_get_interface` harness that throws malformed id/version pairs at the
resolver.

### 6. Crash isolation proves survival but conditionally skips the classification check

`ws_crash.test.mjs` is the headline crash-isolation test. Its strong assertion is
correct and load-bearing: after each of null-deref / div-zero / array-overrun /
C++-throw, it pings and asserts the backend still answers
(`ws_crash.test.mjs:111-112`). But the assertion that the crash was *detected and
classified* — `log.msg.includes(crash.expect)` for e.g. `ACCESS_VIOLATION` — is
wrapped in `if (result.errorLogs.length > 0)` (`ws_crash.test.mjs:102-108`).
`result.ran` is captured but never asserted (`ws_crash.test.mjs:68, 101`), and
the error logs are gathered after a fixed `await sleep(500)`
(`ws_crash.test.mjs:65`).

#### Consequence

If the SEH translator regressed to swallow a fault silently — process stays up,
but no classified error reaches the operator channel — this test stays green: the
`errorLogs` array is empty, so the classification assertion is skipped entirely,
and survival still holds. The crash-*diagnosability* net (which testing.md
correctly names as the replacement for the removed process isolation) is thus not
actually asserted in the common path. The `sleep(500)` also makes the log-present
branch timing-dependent, so under load the check can silently downgrade to
survival-only (see finding 8).

Plugin-boundary *throw* isolation is, separately, well covered:
`test_plugin_exception.cpp` proves a C++ throw does not unwind the C ABI both
directly and through the real `CAbiInstanceAdapter`, and reaching the line after
each call is the assertion. Raw-thread crash forensics are covered by
`examples/plugin_crash_forensics`. The gap is specifically the *script* SEH
classification check being optional.

#### Recommendation

Make at least one crash type assert unconditionally that a classified error was
reported (await the specific error log rather than draining whatever arrived in
500 ms), and assert `result.ran` reflects the pre-crash rsp. Survival and
diagnosability are two separate guarantees; both should be asserted, not one
gated behind the other's side effect.

### 7. The canonical testing inventory understates the gate surface roughly four-fold

`README.md` (Testing matrix) advertises "12 targets" for the C++ layer, and
`docs/testing.md:32-48` lists ~11 binaries. The tree registers **44** ctests over
34 `test_*.cpp` files (`add_test` count in `backend/CMakeLists.txt`), including
substantial suites the doc omits entirely — `test_abi_freeze`,
`test_capability_handshake`, `test_emit_gate`, `test_interface_domains`,
`test_metrics`, `test_parallel_safety`, `test_state_migrate`,
`test_record_schema`, `test_prepare_concurrency`, and others.
`testing.md:2-3` explicitly promises "Update on every test addition."

#### Consequence

The one document whose job is to be "the live picture of test surface" is stale,
and stale in a way that makes it useless for the task this review performs:
you cannot tell from it what is actually gated. Here the drift understates
coverage (the suite is broader than advertised), which is less dangerous than
overstating — but a coverage inventory that is wrong by 4× cannot be relied on by
a newcomer, an auditor, or a maintainer deciding whether a surface is protected.

#### Recommendation

Regenerate the C++ table from `backend/CMakeLists.txt` (the `add_test` list is
the source of truth) and add a cheap check — analogous to `check_doc_coverage.py`
— that fails if a registered ctest name is absent from `testing.md`. The project
already trusts derive-from-source gates for docs; apply the same idea to the test
inventory so it cannot rot.

### 8. Flakiness: fixed sleeps, random ports, and acknowledged single-client races

The node/E2E layer leans on wall-clock timing and randomness that can flake
independently of the code under test:

- `ws_crash.test.mjs:65` gathers crash logs after a fixed `sleep(500)`; under CI
  load or a slow debug build the log may arrive later, silently downgrading the
  test (finding 6).
- `ws_crash.test.mjs:17` picks a random port in `[30000, 50000)` with no
  collision retry; two concurrent runs (or a busy host) can clash.
- The fuzz smoke and `run_qa.py` both interleave fixed `sleep`/`wait_port_free`
  delays specifically because the WS server is single-client and the VS Code
  extension races for the default port
  (`tests/fuzz/run_smoke.py:52-55`, `tools/run_qa.py:74`,
  `tests/fuzz/README.md:43-56`). This is handled, but it is timing-shaped
  coordination, not a lock — the failure mode is a spurious 503, not a real bug.

#### Consequence

Timing- and port-based flakiness erodes trust in the exact gates that matter most
(crash survival, backend lifecycle). A flaky gate gets muted, and a muted gate
stops being evidence. Pre-CI this is latent; the moment finding 1 is addressed
and these run unattended, flakiness becomes the first thing that undermines them.

#### Recommendation

Replace fixed `sleep`s in assertions with condition-waits (await the specific
message/log with a generous timeout, rather than sleeping then draining). Add a
bind-retry to random-port selection. These are the standard hardening steps
before wiring the node suites into any automated runner.

## What Is Genuinely Good (keep it)

Recording the strong evidence so a future refactor does not discard it:

- **`xi_run_plugin` + `ImagePool::make_host_api` is a real host-contract mock.**
  A plugin author can drive `process()` against the *actual* pool host_api,
  including the carved v10 interfaces (`get_interface`, `xi.imaging/doc/preview/
  log`), all wired (`xi_image_pool.hpp:377-483`). Only `emit_record` and the WS
  sinks (status/binary/compress) are headless no-ops. That is a usable,
  faithful test seam, uncommon at this stage.
- **Failure injection is authentic.** `plugin_crash_forensics` crashes the
  process from an unmanaged plugin thread and asserts minidump + `last_phase`
  breadcrumb survive; `test_plugin_exception.cpp` proves ABI non-unwind through
  the real adapter; `test_set_def_race` / `test_prepare_concurrency` /
  `qa_reentrancy` prove the reentrancy admission gate with concrete
  max-concurrency assertions (1 / ≥2 / 1). These are fault tests, not smoke.
- **The additive run-outcome fixtures are sound.** `run_result.json`,
  `run_result_crashed.json`, `run_result_no_verdict.json` match the *implemented*
  identity contract (Bucket A landed) and are meaningfully parsed and classified
  by the Python client (`test_run_outcome.py:43-99`). This is the correct model
  the partial-status fixtures (finding 2) should follow.
- **The derive-from-source doc gates** (`check_doc_coverage.py`,
  `check_retired_terms.py`) are a real integrity net: they compute the public
  surface from the source of truth and fail on undocumented-live or
  present-retired symbols, re-verifying the retired tokens against
  `backend/include/xi` each run. Apply the same pattern to the test inventory
  (finding 7).

## What Is Not Tested (by design or by omission)

Called out explicitly so the gaps are chosen, not discovered in the field:

- **Hot-reload *cycles* under load.** A single swap is covered
  (`test_hot_reload_swap`, `ws_reload_verify`); repeated reload-during-dispatch
  churn (state serialize/restore stability over N cycles) is not.
- **Multi-camera correlation failure modes.** `runMulticam` proves the happy path
  (synced_stereo pairs L+R under one tid). A *dropped* half, a skewed/straggler
  frame, or a correlation timeout — the cases that actually break a stereo line —
  are untested.
- **Crash *recovery* of in-flight state.** Survival is proven; what happens to the
  interrupted frame's verdict, queue position, and instance lock after a caught
  crash is not asserted end-to-end.
- **The not-yet-emitted partial-status path** (finding 2) — untested against a
  real backend by construction.
- **Long-run soak.** Explicitly retired (review 05 #23); no memory/thread/handle
  plateau evidence. Acceptable pre-1.0, but it means "24/7 stable" is unproven.
- **Linux.** Entirely untested (Windows-first SEH, `cl.exe` driver, `PrintWindow`
  E2E).

## Prioritized Roadmap

### P0 — Stop green from lying

1. Re-point or relocate the `commit_group_*` / `load_project_partial` fixtures
   and their tests so they bind to an emitter that reproduces them (finding 2).
2. Delete the orphaned `vars_mixed.json` and fix the `repository-map.md` row
   (finding 3).
3. Make one crash type in `ws_crash` assert classification unconditionally
   (finding 6).

### P1 — Enforce and extend

1. Add a single `run_all` script and a minimal push job that runs the socket-free
   ctest subset + node suites; wire in the fuzz smoke at smoke budget (findings
   1, 5).
2. Extend `protocol.test.mjs` to shape-check the run-outcome / metrics fixtures
   for C++↔TS parity (finding 4).
3. Regenerate `testing.md`'s C++ table from `add_test` and add a
   test-inventory freshness check (finding 7).

### P2 — Harden and fill

1. Replace assertion `sleep`s with condition-waits; add port-bind retry
   (finding 8).
2. Add a `get_interface` fuzz harness (finding 5).
3. Add a multi-camera correlation *failure-mode* test (dropped half / skew)
   and a hot-reload-cycle soak-lite (What Is Not Tested).

## Decision Checklist

- **Enforcement:** Is there anything that runs the gates without a human? (Today:
  no.) What is the minimum automated subset worth enforcing pre-1.0?
- **Fixture binding:** For each fixture, name the emitter contractually bound to
  reproduce it. If none exists, the fixture is a mock of an intention, not a test.
- **Skip semantics:** Which assertions are guarded by an `if` that can silently
  vanish? A conditionally-skipped assertion is not coverage.
- **Parity surface:** Which wire events does the extension decode that have no
  C++↔TS parity fixture?
- **Fuzz cadence:** Who runs the fuzz tiers, how often, and what happens on a
  finding? If the answer is "nobody, never," it is documentation, not a net.
- **Inventory trust:** Can a newcomer determine what is gated from the docs alone?
  (Today: no — 4× undercount.)

## Final Judgment

xInsp2's test *engineering* is better than its test *evidence*. The core unit
layer, the failure-injection tests, and the plugin host mock are the work of
someone who understands what a test is for — they inject real faults and assert
real invariants. That foundation is sound and should be praised.

The problem is a small number of tests that are green while proving nothing —
and, worse, proving nothing about the exact contracts (partial commit, partial
restore, protocol parity, crash classification) a reader would most want assured.
Combined with the absence of any enforcement and a test inventory that is
4× stale, the *aggregate signal* the suite sends is louder and more confident
than what it has actually verified.

The corrective work is cheap and mostly subtractive: delete the orphan, re-bind
or relocate the aspirational fixtures, un-skip one assertion, add one `run_all`
script, and regenerate one table. None of it touches the core. Doing it converts
the suite from "broad, and mostly honest" to "broad, and trustworthy" — which,
for a framework selling crash-safety and a frozen ABI, is the property that
matters more than breadth.
