# Known issues / open bug list

Snapshot after the 2026-06 hardening campaign (two structural-review passes; the
fixed items merged to master in `17384e4`). This lists what's **still open**, ranked
within each group. Each is tagged with the design theme it belongs to:
A = invariant left to manual discipline at every call site · B = one fact in many
stores kept in sync by hand · C = lifetime/ownership across threads with no
structural owner · D = liveness/observability at the wrong boundary · E = a
load-bearing contract that lives only in a comment/convention.

## Correctness — latent / edge (real, but not currently reachable or needs a rare race)

- **F4 — lane group config is a copy, not a live view** (theme B). `GroupLane.cfg`
  is copied from `project_.groups` at `spawn_group_pool_`; routing (`lane_for_`)
  reads the *live* `default_group` but matches the lane's *copied* `cfg.name`. If a
  group is reconfigured in the model without a stop+respawn, events route to the
  wrong group. **Latent:** groups are load-only — no runtime "reconfigure groups"
  command exists today (same shape as the a64c073 merge-allowlist case). Fix when a
  runtime reconfigure is added: lane holds an immutable shared snapshot of the whole
  `groups` + `default_group`.
- **F5 — working-copy "who is authoritative" has no lock** (theme C/B). Opening a
  working copy rebases all writes to `.xinsp_work` purely by convention; nothing
  stops a second backend (or the same one with `working_copy:false`) from writing the
  canonical concurrently, and a later commit's `mirror_tree` would clobber those
  writes. **Edge:** needs a concurrent second writer. Fix: an advisory lock file
  (PID+timestamp) at the canonical root, detected on open.

## Observability — unattended-PC signal gaps

- **P1-3 — plugin `host->log` / stderr reaches no operator channel.** `api.log`
  (even ERROR) only `fprintf(stderr)`; on an unattended PC stderr is unwatched, so a
  plugin's self-diagnostics are invisible. Route ERROR/WARN to the WS log + maybe
  crash history.
- **P1-4 — mid-run recompile failure runs the old def with no persistent degraded
  flag** (theme D). A failed `compile_and_load` replies `ok:false` to the *caller* and
  keeps streaming the last-good DLL, but boot-degraded has a marker and mid-run has
  no equivalent. Add `last_compile_ok` / `running_def_epoch` to `cmd:status`.
- **P1-7 — script `status()` dropped when no client is connected.** No persistent
  landing spot; live-WS-only. Buffer the latest, or stamp to fe-status.
- **P1-8 — drop/high-watermark counters reset at `cmd:start`.** A restart erases the
  "how much did we drop last run" history. Keep a lifetime-cumulative alongside (like
  ImagePool's `total_created_`).

## Design — DO-LATER (real impact, needs design / has a cost)

- **Input image is a writable shared pool buffer, read-only only by convention**
  (theme B, pixel level). After dedup, multiple consumers alias the *same* handle and
  `image_data()` hands back a writable pointer with no COW — a plugin doing an
  in-place op on its input (e.g. `cvtColor(in,in,…)`) silently corrupts the frame for
  every other consumer. Full per-frame COW violates speed-first; the
  low-cost path is a const-input ABI (`image_data` returns const for inputs, output
  forced through `image_create`) — needs an ABI bump + migrating any in-place plugin.
- ~~**canon-dedup mapping rebuilt independently on backend + each client**~~
  **(MOOT — branch `refactor/remove-var-core`).** This concerned the `vars` frame +
  binary preview dedup path, which has been removed from core (image preview is
  moving to a preview plugin). Left here for history; revisit only if the preview
  plugin reintroduces backend-side gid dedup.
- **Non-finite sentinel / json-escape replicated per producer-consumer** (theme A).
  C++ is mostly centralized, but each plugin's `io.hpp` + the JS side re-implement it;
  a new producer that forgets the sentinel emits a bare `NaN` token and the whole
  frame is dropped. Cheap guard: a debug-build assert in the yyjson write path that
  fires on a bare `NaN`/`Inf` token; longer-term a single shared helper.

## Minor / residual

- **F7** — `TriggerEvent` drop-paths (lane overflow / teardown drain) still release
  by hand; `CurrentTriggerScope` only covers the run-sites.
- **F8** — `spawn_group_pool_` doesn't assert "already stopped" (all callers pair it
  today, by convention).
- **ws_fallback_gate.test.mjs** is skipped (no no-export fixture) → the yyjson-layout
  gate is unguarded by CI.
- **`xi_inspect_entry(int frame)` hint wraps at 2^31 frames** (~248 days @ 100 fps,
  ~25 days @ 1 kfps). Diagnostic only — the wire `run_id` is int64 and correct, and
  the crash breadcrumb `last_run_id` is now int64 too (fixed). Widening the *frame
  hint* to int64 is deferred because it's the user-facing script entry signature
  (every script + every doc example + the new-project template write
  `void xi_inspect_entry(int frame)`); not worth a script-ABI churn for a hint that
  only goes negative after months of 24/7 uptime. Do it with the next script-ABI bump.

## Bounded-in-practice (audited 2026-06-27 tax-when-unused sweep — left as-is)

- **`TriggerBus::source_last_emit_mono_us_` never evicts** (xi_trigger_bus.hpp). A map
  keyed by emitter source name, stamped every emit; `evict_stale()` is a deliberate
  no-op. It can't be TTL-evicted because its whole purpose is per-source staleness
  reporting (`source_emit_ages_us` → "which camera stalled") — evicting a stale source
  erases exactly the stall signal. Bounded in practice by the stable source set (a
  fixed camera/emitter roster); only a plugin churning *distinct* source names per
  frame would grow it, and plugins are trusted. Same posture as the g_status case.
- **Per-run hot path audited clean**: graph_capture OFF = one relaxed atomic load;
  result()/status() fire only when the script uses them; EmitTurn/watchdog gated;
  crash breadcrumbs ~100-200ns and necessary. No tax-when-unused worth gating. (The
  one latent idea — a fully headless run, zero clients connected, still did
  per-run snapshot/emit work for nobody — is now moot: the `vars`/preview emit
  path was removed from core entirely (branch `refactor/remove-var-core`).)

## Won't fix (under current policy)

- `frozen_` non-atomic on the Record COW path — the refcount itself is correct
  (copy `fetch_add` relaxed, release `fetch_sub` acq_rel); `frozen_` only races under
  the unsupported "same Record instance touched by two threads" usage. Atomizing it
  doesn't fix the real UB, only TSan noise. Revisit only if parallel doc-sharing
  becomes a real path.
- ~~Per-connection subscription set (multi-client preview stomp)~~ — moot:
  deployment is single-client, and the `subscribe`/preview path was removed from
  core (branch `refactor/remove-var-core`).
- Verifying plugin self-declared `reentrant` / `XI_PLUGIN_STAGED` — plugins are
  trusted; this would be a hostile-plugin defense.

## Pre-existing backlog (still open, predates the campaign)

- God-file refactors: `extension.ts activate()`, `service_main.cpp handle_command()`,
  `xi_plugin_manager.hpp`.
- Per-field NA does not propagate through `xi::Number::value()` scalar downgrade
  (reads as 0) / `require()` on a nested `$na` — needs NA-lifting in `io.hpp`.
- Nesting a sub-Record drops its images (`Record::set(key, subRec)` copies JSON, not
  `images_`) — warns once today; proper fix needs namespacing.
- ~19 ws-test files redefine their own `Client` boilerplate — migrate to
  `helpers/client.mjs`.
- Zombie `shm_*` ABI fields (5 fns, always null) pad the struct; removing them shifts
  every later pointer — now guarded by the `xi_abi.h` `static_assert`, so removal
  must bump `XI_ABI_VERSION`.
- OpenCV discovery hardcodes `C:/opencv` paths (fresh external checkout fails).
