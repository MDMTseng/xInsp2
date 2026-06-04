# Per-run result — status code + message (design)

> **Status: design, not scheduled.** Captures the agreed shape. 2026-06-05.

## Goal

Every inspection run gets **exactly one Result**: a signed status code + a
message + provenance. It's the one record MES / PLC / the HMI verdict-yield-Pareto
cards consume — distinct from `VAR`s (which are rich per-run debug/inspection
values; a run emits many). **Every trigger produces exactly one Result** — no
silent gaps — even when the run never happened (dropped / crashed).

## Status code space

A signed int. **Sign = verdict, magnitude = sub-class**, with a reserved band for
framework-generated failures so they never collide with user codes:

| Range | Meaning | Who sets it |
|---|---|---|
| `> 0` | **OK** class: `ok1`, `ok2`, … | script (`RESULT`) |
| `0` | **NA / none** — no verdict applicable | default if script set nothing |
| `-1 … -989999` | **NG** class: `ng1`, `ng2`, … (user reject reasons) | script (`RESULT`) |
| `≤ -990000` | **SYSTEM fail** (framework-generated; an enum, can be many) | the framework |

Reserving `≤ -990000` answers "a system enum can have multiple framework fails":

```cpp
enum XiSysResult {            // framework-only; never emitted by user code
    XI_SYS_DROPPED    = -999001,   // overflow: queue full, event dropped before inspect
    XI_SYS_NO_TRIGGER = -999002,   // synthetic tick with nothing to inspect
    XI_SYS_CRASHED    = -999003,   // inspect threw / SEH-translated fault
    XI_SYS_TIMEOUT    = -999004,   // watchdog killed the inspect
    XI_SYS_NO_VERDICT = -999005,   // ran to completion but script set no RESULT
};
```

Consumer rule: `code <= -990000` → **system fail** (infrastructure, not a real
reject); `-990000 < code < 0` → **user NG**; `> 0` → **OK**; `0` → **NA**. So the
HMI can show a process reject (ng) differently from "the line dropped a frame"
(system), and yield math can exclude system fails or bucket them separately.

## The key rule: the framework fills the non-run cases

A **dropped** event never runs the script, so the script *cannot* emit its result —
**the dispatcher emits it** at the drop site (e.g. `enqueue_to_lane_` /
`enqueue_event_` overflow → `RESULT = XI_SYS_DROPPED`). Likewise the run loop
synthesizes `XI_SYS_CRASHED` (inspect threw), `XI_SYS_TIMEOUT` (watchdog), and
`XI_SYS_NO_VERDICT` (script ran but called no `RESULT`). This is what guarantees
*one result per trigger* and lets the HMI/MES compute true NA / drop rates instead
of seeing silence.

## Fields

**Core (always):**
- `code` (signed int) — as above
- `message` (string) — human-readable status
- `run_id` — correlation (exists)
- `ts` — wall-clock timestamp
- `cycle_ms` — inspect wall-clock (already on `run_finished`)

**Provenance (recommended):**
- `source` / `group` / `trigger_id` — which source / dispatch group / trigger

**Optional (line / MES / traceability — add later):**
- `part_id` / `serial` — physical part (barcode)
- `recipe` / `program_version` — which inspection program version judged it
- `defects: [{ code, where }]` — for NG: failed features + location (feeds overlays)
- `schema` — result-record version (forward-compat)

## Script API

```cpp
RESULT(code, "message");          // e.g. RESULT(-2, "edge chip > 0.3mm")  → ng2
// or the OK helpers: RESULT_OK("clean"), RESULT_NG(2, "edge chip")
```
First-class (not a `VAR`) so the framework can: default to `0` (NA) if unset,
reject user use of the `≤ -990000` band, and synthesize the system codes itself.

## Flow + relationships

- The Result rides a **dedicated `run_result` event** (decided — *not* folded into
  `run_finished`), so a **dropped** trigger — which has no run / no `run_finished` —
  emits the same shape. `run_finished` stays lifecycle/timing only.
  `{"type":"event","name":"run_result","result":{code,msg,run_id,ms,source,group}}`.
- **HMI**: a `verdict` card binds `code` (sign → green/red; `≤ -990000` → a distinct
  "system" colour); `yield` excludes/【buckets system fails; a **Pareto** card ranks
  by `code`/`message`. Ties into [`production-hmi.md`](./production-hmi.md).
- **comms / PLC**: the gateway reads the Result to decide pass / reject / rework /
  alarm; a system fail (`≤ -990000`) maps to safe-state, not a part reject.
- **dispatch groups**: the overflow drop path is exactly where `XI_SYS_DROPPED` is
  emitted — see [`dispatch-groups.md`](./dispatch-groups.md).

## Increment plan

### Phase 1 — per-run Result (the core; low-risk, standalone value)
1. **Script API** — `xi_result.hpp` (new), mirrors `xi_status.hpp`'s host-callback
   pattern: `xi::result(code, msg="")`, `xi::ok(n=1,m="")` (→ `+n`), `xi::ng(n=1,m="")`
   (→ `-n`). The header **rejects the system band**: a user `code <= -990000` is
   clamped to `-1` + logged (user code can't squat the framework enum).
2. **Script support** — `g_result_fn_` + `xi_script_set_result_callback(void*)` in
   `xi_script_support.hpp` (alongside the existing `set_status_callback`).
3. **Host** (`service_main.cpp`) — `thread_local RunResult{code=0,msg,set=false}`
   (per-lane, like `g_run_frame_path_`); reset before each inspect; result callback
   writes it. After a successful inspect that set nothing → stays `0` (NA) for
   back-compat (**`XI_SYS_NO_VERDICT` is a v1.1 opt-in**, not the v1 default — so
   existing scripts that never call `RESULT` aren't flooded). Emit a **dedicated
   `run_result` event** inside the `EmitTurn` (after vars/previews, before
   `run_finished`).
4. **Drop path** — `enqueue_to_lane_` / legacy overflow emits a `run_result` with
   `XI_SYS_DROPPED` (+ trigger_id/source/group, no run_id) at the drop site →
   one Result per trigger, no gaps.
5. **HMI** — `protocol.mjs` decodes `run_result`; verdict card reads `code` (sign →
   colour; `<= -990000` → system colour); yield uses sign. Minimal wiring.
6. **comms** — *no comms code in v1*; a script can already forward via
   `xi::comms.send(...)`. Gateway directly consuming `run_result` = v1.1.
7. **Test + docs** — `examples/qa_run_result/` (ok/ng/unset → assert
   `run_result.code`; a `queue_depth:1` flooded project → assert `XI_SYS_DROPPED`);
   update `writing-a-script.md` (`VAR` vs `RESULT`).

### Phase 2 — per-group result_order (touches the parallel emit path; separate commit)
- `DispatchGroup.result_order` (default `"completion"`; `"arrival"` = ordered).
- **Generalise `EmitTurn`** so the gate targets either the global cursor (legacy
  pool) or a **per-`GroupLane`** one (`emit_seq_next` / `emit_cursor` / `emit_mu` /
  `emit_cv`). Lane assigns `emit_seq` at dequeue under `lane->mu` (arrival order);
  the worker wraps `run_result`+`run_finished` in a lane-scoped `EmitTurn`. Compute
  stays fully parallel; only emission serialises, per group. Stop/drop advances the
  cursor (reuse the existing skip-on-stop logic) so an ordered stream can't wedge.
- Test: `qa_dispatch_groups` Test E — an `arrival` group, deliberately out-of-order
  worker timing → assert `run_result` `run_id` is monotonic.

### Later
- `XI_SYS_CRASHED` / `XI_SYS_TIMEOUT` synthesis; `na_reason` text; Pareto card.
- `part_id` / `recipe` / `defects` traceability fields; MES export; gateway
  consuming `run_result` directly.

## See also
- [`production-hmi.md`](./production-hmi.md) — verdict / yield / Pareto cards.
- [`dispatch-groups.md`](./dispatch-groups.md) — the overflow drop site that emits `XI_SYS_DROPPED`.
- [`guides/writing-a-script.md`](../guides/writing-a-script.md) — `VAR` vs `RESULT`.
