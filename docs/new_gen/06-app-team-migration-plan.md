# Migrating to `polaris_master` — Plan for the App Team

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Audience** | The app-dev team consuming the xInsp2 backend, WS protocol, project files, and shipped clients |
| **Scope** | Everything on `polaris_master` that is not on `master` (25 merged task branches), plus the ONE wire change still pending after it |
| **Status** | Proposal — phases 1–3 need your sign-off; phase 0 you can start today |

## TL;DR

`polaris_master` was engineered to be **wire-compatible**: no message was
renamed, no field changed type, no reply shape moved. Your clients should run
unchanged against it. What you must actually check is four narrow things —
scripts using raw OpenMP pragmas, external tools that write `project.json`,
any third-party XEX1 decoder you own, and scripts that feed plugins malformed
input and relied on silence. Everything else is additive surface you can adopt
at your own pace, plus a large amount of hardening you get for free.

The previously staged breaking set (`integration/extreview-breaking` — wire
renames, `XI_SYS_CRASHED`, HMI throughput semantics, partial-status returns)
is **already merged to master** (merge `86e0a53`) — if your app runs against
current master, you have absorbed it. The only wire change still pending
anywhere is the `hello.abi` bump (Phase 3, coordinated).

## What Polaris is (one paragraph)

Eleven external reviews (docs/ext_review/01–11) were distilled into a
north-star architecture (docs/new_gen/01–02) and an adoption map
(docs/new_gen/03). `polaris_master` executes adoption items 1–14: every
boundary is now either **generated from one source** (contract/ schemas →
validation gates) or **guarded by a gate** (tools/gate.py: docs → build →
ctest → fixtures → qa → fuzz, all green at HEAD). Full status:
docs/new_gen/03-adoption-map.md § Status.

## Impact on you, classified

### A. Action required (check BEFORE merging — the only 4 candidates)

| # | Change | Who is affected | What to do |
|---|---|---|---|
| A1 | **Raw OpenMP pragmas in scripts are rejected at compile** (XI9001, with a message routing to `xi::parallel_for` / `xi::async`) | Any `inspect.cpp` using `#pragma omp` directly (not via the xi wrappers). A faulting raw-omp thread kills the whole backend untranslated — that's why | Grep your project scripts for `#pragma omp`. Port to `xi::parallel_for`/`xi::async` (usually mechanical), or set `"allow_raw_omp": true` in project.json if you accept the worker-thread rules |
| A2 | **`project.json` gains `"schema": "xi.project/1"`, written on every save** | Any external tool of yours that parses project.json and rejects unknown top-level keys, or rewrites the file from partial knowledge (the backend no longer does — yours shouldn't either) | Confirm your tooling tolerates the new key. Files without the stamp load fine (legacy, logged once); a future-major stamp is refused with a precise error |
| A3 | **XEX1 frames can emit msgpack `map16`/`map32`** once the top-level map exceeds 15 keys (today's real frames are byte-identical — still fixmap) | Only a third-party XEX1 decoder you wrote yourself. The in-tree decoders (expose webUI, `examples/lib/xex1.py`) are updated and pinned by golden fixtures under `protocol/fixtures/binary/` | Test your decoder against the goldens (they include a 17-key `map16.bin`); add the two opcodes if missing |
| A4 | **Plugins fail loud on bad input** (`mock_camera`, `blob_analysis`): a missing/mis-typed required key now returns a structured NA (reason code + key + expected type) instead of a silent default; unknown keys warn once | Scripts that feed these plugins incomplete Records and relied on silent tolerance | The structured NA maps to your existing verdict handling; fix the call site (the error names the exact key). Happy-path callers see zero change |

### B. Observable behavior fixes (no action expected — but be aware)

- **A handler exception now becomes `rsp ok:false`** correlated to your command
  id, instead of killing the backend process. (The documented contract is now
  true.)
- **A malformed command envelope with a recoverable `id` gets a correlated
  error reply** instead of a log-only silent drop — your client no longer
  blocks to its timeout on a typo'd field. Rejects are counted in
  `dispatch_stats.malformed_cmd_rejected_lifetime` (new field).
- `get_dashboard` / `crash_reports` cap file reads at 8 MiB
  (`truncated:true` beyond that) instead of attempting unbounded allocation.
- `save_project` **preserves every top-level key it doesn't own** (previously
  it silently rebuilt the file from two keys — if you ever lost `runtime` /
  `parallelism` / `groups` after a save, that's fixed).
- A second WS client is still refused; through the HMI proxy the refusal is
  now an explicit close code (4003) so the HMI shows "another client is
  connected" instead of generic retry.

### C. Additive surfaces you can adopt when useful (ignore = zero cost)

| Surface | What it gives you |
|---|---|
| `get_health` cmd + `health_changed` event (schema `xi.health/1`) | The canonical backend state machine (boot → project_loaded → running ⇄ degraded / draining / fault) + per-component health with reason codes — stop inferring liveness from side channels. Feature-detect via "unknown command" on old backends |
| `fe-status.json → be_health` | The supervisor now mirrors the BE's canonical state (file-based, doesn't consume the single WS client slot) — your line-side watchdogs can read it |
| `on_fault` policy (plugin.json / instance.json): `reuse` (default) / `reinit` / `refuse` | Explicit post-crash reuse policy per plugin; escalation to quarantine after 3 failed re-inits; re-enable by re-committing config |
| Python SDK 0.3.0 (`tools/xinsp2_py`) | `--auth` support (bearer + timestamp-HMAC), `AuthError`, contract-typed run outcomes. Note: no auto mode-fallback by design (a bearer-first fallback would leak an HMAC key) |
| Extension (dev client) | Run-verdict status bar, `state_dropped` warnings, compile lifecycle, version-skew warnings, health chip |
| `compat-manifest.json` in every release artifact | Machine-readable identity: backend version, plugin ABI (v11), WS command count, schema list, tested-together client versions. A target machine answers "what am I running" from the artifact alone |
| `contract/` schemas + fixtures | The wire, machine-readable. If you build a consumer, generate/validate against these instead of reading prose |
| Builder/extractor headers (`<plugin>_io.h`, `<plugin>_keys.h`) | Typed, compile-checked plugin I/O for scripts — key typos become compile errors |

### D. Free hardening (invisible unless you were being bitten)

Pool-stats UAF fix (a UI polling stats can no longer fault the backend), WS
teardown race fix, crash-breadcrumb slots recycle (long-run one-shot dispatch
no longer degrades crash attribution), post-fault health overlay, hot-path
benchmark with honest labels.

## Migration phases

### Phase 0 — inventory + test rig (you, now; no repo changes)

Answer the four A-items against your codebase:

- [ ] `grep -r "#pragma omp" <your projects>/… ` — list scripts to port (A1)
- [ ] List every tool of yours that READS or WRITES `project.json` (A2)
- [ ] Do you own any XEX1 decoder outside this repo? (A3)
- [ ] Any script that knowingly under-feeds `mock_camera`/`blob_analysis`? (A4)
- [ ] Confirm your WS client ignores unknown `event` names and unknown fields
      (it should already — the protocol has been additive-evolving all along)

Then point one line/test rig at a `polaris_master` build and run your app's
own regression pass. We can hand you a zipped artifact (with its
`compat-manifest.json`) on request.

### Phase 1 — the merge (us + you, one step)

`polaris_master` → `master` as ONE merge (25 branches, already integrated and
gate-green; no cherry-picks — the branches interlock). Prerequisites:

- [ ] Phase 0 checklist clean or remediated
- [ ] Your app's regression pass green on the rig
- [ ] `tools/gate.py` green at the merge commit (we run it; you can too — it
      is the same command CI runs)

Rollback story: pre-merge `master` stays tagged; artifacts are zip-swappable
and self-identifying via the manifest. The merge itself is trivially
revertible in git (one merge commit).

### Phase 2 — adopt at your pace (you, whenever)

The C-table above, in whatever order pays for itself. Recommended first:
`be_health` in your line-side monitoring (cheap, file-read), then `get_health`
in whatever operator surface you own.

### Phase 3 — the `abi` bump cutover (us + you, coordinated, LAST)

The one remaining wire change anywhere: `hello.abi` goes 1 → 2, in lockstep
with `EXPECTED_WS_ABI` in the extension. This is deliberately NOT in
`polaris_master`. Mechanics are already in place on both sides:

- the **baseline gate** (`contract/baseline_gate.py`, a ctest) now refuses any
  breaking schema change without a version bump — so "did the wire move" is
  machine-checked, not remembered;
- the extension already **enforces** the stamp (treats `abi != 1` as
  incompatible with a visible warning).

At the cutover we bump the stamp, refresh the baseline, and your clients that
check `abi` (if any — today none of yours do, which is exactly the problem
this fixes) start getting a real signal. If you own a WS client, add the
check then: read `hello.data.abi`, compare to the value you were built
against, warn loudly on mismatch. One comparison.

## Known issues shipped (honesty section)

- 4 QA examples are quarantined as pre-existing failures
  (`examples/qa_known_failing.txt`, each with its reason):
  `qa_lifecycle_teardown`, `qa_local_auto` (driver expects retired VAR-era
  auto-emit), `qa_recipe_script_instance`, `qa_reentrancy` (flaky on master
  base). They fail on master too — quarantine makes them loud instead of
  gate-blocking. A deflake/behavioral pass owns them.
- `ws_teardown_race` (a stress test, not product code) flakes rarely under
  load; same owner.
- The GitHub Actions workflow is authored but has never run on a hosted
  runner (no remote CI yet); `tools/gate.py` locally is the enforced truth.

## What we need from you

1. Phase 0 checklist results (especially the A1 script inventory).
2. A named contact + a rig/window for Phase 1.
3. Your preference on Phase 3 timing: piggyback on your next planned client
   update, or schedule standalone. There is no urgency — the gate prevents
   drift while it waits — but every release that ships before it is one more
   deployed client that ignores the version field.

## Reference

- Full change inventory: `git log master..polaris_master --first-parent`
  (25 merges, each message summarizes its branch)
- Reviews that motivated each change: docs/ext_review/06–11
- Architecture rationale: docs/new_gen/01 (north star), 03 (adoption map +
  status), 04 (health contract), 05 (schema language)
- Wire truth: contract/schemas + protocol/fixtures (text),
  protocol/fixtures/binary (XEX1 goldens)
