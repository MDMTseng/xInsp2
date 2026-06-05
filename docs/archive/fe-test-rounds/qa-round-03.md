# QA team loop — Round 3 (countdown 8 → 7) · targeted: FE boot-readiness gate

Targeted round: the achievable slice of FE-E4 (the "hang" path round 1 left as a
stub).

## Gap
The FE's liveness probe is a shallow TCP `connect`. The backend binds its WS
port, then runs open/compile/start *synchronously* for several seconds before
serving — and a `connect` succeeds the whole time (kernel completes the
handshake even with no `accept`). So a backend that **hangs during boot** (never
finishes compiling / never serves) looks "healthy" to the FE forever. The
connect probe fundamentally can't distinguish booting from serving.

## Fix (product)
- **Boot-readiness gate** in `fe_main.cpp`: when running a `--project`, the FE
  now withholds "healthy" until the BE log shows `autostart: ready`. If the BE
  is alive but hasn't reached ready within `--boot-timeout-ms` (default 60 000),
  the FE declares a **boot hang** → `enter_safe_state(BootTimeout)` → kill →
  respawn. New `SafeStateReason::BootTimeout`.
- New FE flags: `--boot-timeout-ms`, and `--be-arg=ARG` (repeatable) — appends
  args verbatim to the spawned backend (operator passthrough; also how the test
  injects the hang).
- BE debug hook `--hang-before-ready` (test-only, like `--test-crash`; not in
  `--help`): sleeps before printing `autostart: ready`, simulating a boot hang
  (alive, port bound, never ready).

## Test
- `examples/qa_race/driver_boot_hang.py`: FE forwards `--hang-before-ready` to
  every BE via `--be-arg`, with `--boot-timeout-ms=4000`. Asserts the FE logs
  the boot-timeout, drives `BootTimeout` safe-state, respawns, hits the cap, and
  leaves no orphan. **PASS.**
- `test_safe_state` SS-U1 extended for the `BootTimeout` mapping.

## Result
- New driver PASS; `test_safe_state` PASS; ctest **11/11**.
- Regressions all green after the monitor restructure: `fe_supervisor`,
  `fe_supervisor_healthy`, `qa_func`, `qa_race`, `qa_fault`, `qa_edge`.

## Still deferred (Phase 2)
A backend that wedges **while serving** (port stays bound + accepting, but
commands stall) still can't be caught by a connect probe — that needs a real WS
handshake/ping heartbeat (`driver_fe4.py` stays a documented stub). The boot
gate covers the boot-time half; the serve-time half is the deep-heartbeat item.

## Dryness check
After 3 targeted rounds the known concrete gaps are closed except the Phase-2
deep heartbeat (a feature, not a quick fix). Round 4 should verify dryness:
re-sweep for any NEW corner case; if none actionable short of Phase 2, stop.
