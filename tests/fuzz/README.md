# tests/fuzz — black-box fuzz smoke (salvaged r7/r8)

Black-box fuzz harnesses that drive the **real** `xinsp-backend.exe` over
its WebSocket protocol and assert it never crashes, hangs, or silently
drops a command under malformed / concurrent input.

This is **Tier 0 item T0.2** from `docs/internals/core_fix_plan.md`
(Part IV §23, §25): salvage the still-valid r7/r8 fuzz harnesses, drop
the obsolete ones, and promote them from one-shot `qa/` surveys
into a maintained, reduced-iteration smoke that can be wired into CI as a
build-breaking net (Invariant §27.4).

These are Python/script only. The backend exe is produced by the normal
backend build; nothing here adds a CMake target.

## Quick start

```sh
# one-shot smoke — small budgets, all kept harnesses, ~45s
python tests/fuzz/run_smoke.py

# heavier sweep
FUZZ_ITERS=1000 FUZZ_DURATION=20 python tests/fuzz/run_smoke.py

# a single harness directly
FUZZ_ITERS=200 python tests/fuzz/harness_ws_cmd.py
```

`run_smoke.py` exits non-zero if any harness reports a **fatal** (backend
crash, lost command, or unrecoverable stall). Per-run details land in
`_results_<name>.json` and `_run_smoke_summary.json` (git-ignored).

### Knobs (env)

| Var | Effect |
|-----|--------|
| `FUZZ_ITERS` | iteration count for iteration-based harnesses |
| `FUZZ_DURATION` | wall-clock seconds for the storm harnesses |
| `FUZZ_SEED` | RNG seed (reproducible payloads) |
| `XINSP_BACKEND_EXE` | path to the backend exe (default: `backend/build/{Release,Debug}/xinsp-backend.exe`) |
| `XINSP_WS_PORT` | WS port to spawn the backend on and connect to (default `7823`) |

### Single-client server / dev-box note (read this)

The WS server is **single-client by design** (`single-client-busy`, HTTP
503 to a second connection). On a developer box the **VS Code extension
auto-grabs the slot on the default port `7823`** the instant any backend
opens it, which races — and 503s — a fuzz client. If you are running
locally with VS Code open, run the smoke on a free port the extension is
not watching:

```sh
XINSP_WS_PORT=7824 python tests/fuzz/run_smoke.py
```

In CI (no extension) the default `7823` is fine.

## What was salvaged vs dropped (and why) — §25

Source: two unmerged survey branches, neither merged:
`origin/feature/fl-r7-fuzz` (`0c7b32a`) and
`origin/feature/fl-r8-concurrency-fuzz` (`8cb5976`).

### KEPT (ported here — still-valid surfaces, healthy when surveyed)

| harness | targets | original verdict |
|---------|---------|------------------|
| `harness_ws_cmd.py` (r7 #1) | WS `cmd` JSON parser (`service_main.cpp::handle_command` → `xp::parse_cmd`) | 1500+ iters, 0 crashes; one P1 accept-stall (single-client) |
| `harness_config.py` (r7 #2) | project / instance / plugin manifest validation | 0 findings |
| `harness_emit_trigger.py` (r7 #4) | `emit_trigger` / RPC in-process path via `exchange_instance` | 0 findings |
| `harness_emit_x_cmd.py` (r8 #1) | emit-stream vs WS-cmd contention | healthy |
| `harness_cmd_during_compile.py` (r8 #3) | commands in flight while a compile parks the handler | healthy |
| `harness_set_param_storm.py` (r8 #5) | `set_param` storm during continuous mode | healthy |

These exercise surfaces that **still exist** in the current
single-process backend, so they keep their regression value.

### DROPPED (targeted the removed-2026-05 process-isolation layer)

| dropped | why |
|---------|-----|
| r7 `harness_evil_worker_host.py`, `evil_worker.cpp`, the `evil_worker` CMake target, `_control_evil.py` | drove the IPC frame parser / `ProcessInstanceAdapter` / `xinsp-worker.exe` — **that code no longer exists** (SHM / process isolation removed 2026-05; §5.1 in-process bet) |
| r8 `harness_backend_kill.py`, `harness_open_close_cycle.py` | worker-process lifecycle (orphan-worker cleanup, open/close churn of out-of-process instances) — same removed layer |

The dropped harnesses cannot run against the current core because the
host they fuzzed (`xinsp-worker.exe` + the named-pipe IPC) is gone.

## ⚠️ The r7 FRICTION_FUZZ "CRITICAL" P0 is NOT a live finding

`origin/feature/fl-r7-fuzz`'s `FRICTION_FUZZ.md` reports a **"CRITICAL"
P0**: an IPC reader thread in `xi_process_instance.hpp` that aborts the
host via `std::terminate` on a worker EOF. **That verdict is historical.**
It was real *in its era* (and was addressed by `fix/r7-p0-reader-disconnect`),
but it targets the **process-isolation IPC layer that was removed in
2026-05**. There is **no `xi_process_instance.hpp`, no `xinsp-worker.exe`,
and no IPC reader thread** in the current single-process backend.

Per **core_fix_plan.md Invariant §27.3** — *"a survey against removed
code is not a live finding"* — do **not** treat that P0 as an open issue
against the current core. It is carried here only as provenance.

## Protocol-drift fixes applied during salvage

The harnesses were de-drifted to run against the current backend:

- **Single `_common.py`.** The originals lived in two `qa/`
  folders; r8's `_common.py` imported r7's by path. Merged into one
  module under `tests/fuzz/`.
- **`next_vars` removed → `drain_events`.** The per-event VAR model was
  deleted from core (VAR became the `expose` plugin; the SDK `Client`
  no longer has `next_vars`). The vdrain threads now call a
  `drain_events()` helper (drains binary frames + the event inbox) — the
  drain only ever existed to avoid unbounded inbox growth under load.
- **Backend exe discovery.** Auto-locates `Release` then `Debug` under
  `backend/build`, overridable via `XINSP_BACKEND_EXE`.
- **`--port` / `XINSP_WS_PORT` support.** So the smoke can dodge the
  single-client contention with the VS Code extension (see above).
- **ws_cmd liveness probe.** Now closes the fuzz socket before opening
  the probe socket (the server is single-client), so the probe no longer
  503s itself — that self-inflicted 503 was the r7 P1 "accept-stall"
  artifact, not a backend bug.
- **Removed the original `.fl_progress` cron breadcrumbs** (they served
  the one-shot survey's parent monitor); progress is plain stdout now.

## Last verified smoke (this branch, Windows)

Run on `XINSP_WS_PORT=7824` against `backend/build/Release/xinsp-backend.exe`:

```
ws_cmd              rc=0  findings=1  fatals=0
config              rc=0  findings=0  fatals=0
emit_trigger        rc=0  findings=0  fatals=0
emit_x_cmd          rc=0  findings=1  fatals=0
cmd_during_compile  rc=0  findings=2  fatals=0
set_param_storm     rc=0  findings=2  fatals=0
total fatals = 0
```

Non-fatal `findings` are benign (stats records, a recovered send-retry,
and a `set_param_throughput_low` note caused by fuzzing unknown param
names — every call still returned, the backend stayed alive).
