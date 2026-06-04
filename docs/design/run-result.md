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

- The Result rides on `run_finished` (or a dedicated `result` message) carrying the
  core + provenance fields.
- **HMI**: a `verdict` card binds `code` (sign → green/red; `≤ -990000` → a distinct
  "system" colour); `yield` excludes/【buckets system fails; a **Pareto** card ranks
  by `code`/`message`. Ties into [`production-hmi.md`](./production-hmi.md).
- **comms / PLC**: the gateway reads the Result to decide pass / reject / rework /
  alarm; a system fail (`≤ -990000`) maps to safe-state, not a part reject.
- **dispatch groups**: the overflow drop path is exactly where `XI_SYS_DROPPED` is
  emitted — see [`dispatch-groups.md`](./dispatch-groups.md).

## Increment plan

- **v1** — `RESULT(code,msg)` API + `xi::Result{code,message,run_id,ts,cycle_ms,
  source,group}` on `run_finished`; framework synthesis of `XI_SYS_DROPPED` (drop
  path) + `XI_SYS_NO_VERDICT` (ran, none set); HMI verdict/yield read `code`.
- **v1.1** — `XI_SYS_CRASHED` / `XI_SYS_TIMEOUT`; Pareto card; `na_reason` text.
- **Later** — `part_id` / `recipe` / `defects` traceability fields; MES export.

## See also
- [`production-hmi.md`](./production-hmi.md) — verdict / yield / Pareto cards.
- [`dispatch-groups.md`](./dispatch-groups.md) — the overflow drop site that emits `XI_SYS_DROPPED`.
- [`guides/writing-a-script.md`](../guides/writing-a-script.md) — `VAR` vs `RESULT`.
