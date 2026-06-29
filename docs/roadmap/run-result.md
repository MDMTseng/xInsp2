# Per-run result — status code + message (design)

> **Status: Phase 1 + Phase 2 shipped.** 2026-06-05.
> Phase 1 (`RESULT` API + `run_result` event + `XI_SYS_DROPPED` at the drop site)
> and Phase 2 (per-group `result_order` via a per-lane `EmitGate`) are both built +
> regression-tested (`examples/qa_run_result/` + `qa_dispatch_groups` Test E).

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

A user `xi::result` code in the reserved band (`≤ -990000`) is **not** silently
accepted: the host records the run as NA (`0`), emits a `warn` log naming the
offending code, and preserves it in the result message (`"[invalid result code
-999001, reserved band] …"`). So the framework enum stays the framework's *and*
the script bug is visible — it doesn't masquerade as a real `ng1`.

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
xi::ng(2, "edge chip > 0.3mm");   // code -2
xi::ok(1, "clean");               // code +1
xi::result(-2, "edge chip");      // equivalent to xi::ng(2, "edge chip")
```
First-class (not a `VAR`) so the framework can: default to `0` (NA) if unset,
reject user use of the `≤ -990000` band, and synthesize the system codes itself.

## Flow + relationships

- The Result rides a **dedicated `run_result` event** (decided — *not* folded into
  `run_finished`), so a **dropped** trigger — which has no run / no `run_finished` —
  emits the same shape. `run_finished` stays lifecycle/timing only.
  `{"type":"event","name":"run_result","data":{"code":C,"msg":"…","run_id":R,"ms":D,"source":S,"group":G}}`
  (fields ride directly in the event `data`, same envelope as `run_finished`;
  `run_id`/`ms` are omitted on a dropped frame).
- **HMI**: a `verdict` card binds `code` (sign → green/red; `≤ -990000` → a distinct
  "system" colour); `yield` excludes/buckets system fails; a **Pareto** card ranks
  by `code`/`message`. Ties into [`production-hmi.md`](./production-hmi.md).
- **PLC / MES**: a plugin or script can act on the Result to drive pass/reject/rework/alarm;
  a system fail (`≤ -990000`) maps to safe-state, not a part reject.
- **dispatch groups**: the overflow drop path is exactly where `XI_SYS_DROPPED` is
  emitted — see [`dispatch-groups.md`](../internals/dispatch.md).

## Increment plan

### Phase 1 — per-run Result ✅ shipped
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
   `run_result` event** inside the `EmitTurn` (before `run_finished`). (The
   `vars`/preview emission that used to precede it in the turn was removed —
   branch `refactor/remove-var-core`.)
4. **Drop path** — `enqueue_to_lane_` / legacy overflow emits a `run_result` with
   `XI_SYS_DROPPED` (+ trigger_id/source/group, no run_id) at the drop site →
   one Result per trigger, no gaps.
5. **HMI** ✅ shipped — `app.mjs` decodes the `run_result` event into `state.result`;
   the **verdict** card shows the code's bucket (OK / NG / NA / SYS colour) + the
   message line, and the **yield** card counts OK/NG/NA from it. A card opts in with
   `bind:{result:true}` (or no `var`); the demo dashboard + `hmi/demo` use it.
6. **PLC / MES integration** — deferred; a plugin handles forwarding the result to
   external systems. Direct gateway consumption of `run_result` is v1.1.
7. **Test + docs** — `examples/qa_run_result/` (ok/ng/unset → assert
   `run_result.code`; a `queue_depth:1` flooded project → assert `XI_SYS_DROPPED`);
   update `write-a-script.md` (`VAR` vs `RESULT`).

### Phase 2 — per-group result_order ✅ shipped
- `DispatchGroup.result_order` (default `"completion"`; `"arrival"` = ordered),
  parsed + validated (unknown value → warn + completion).
- `EmitTurn` was generalised to an `EmitGate{mu,cv,next}` it points at — the legacy
  pool uses one global gate (`g_global_gate`); each `GroupLane` owns its own
  (`GroupLane::gate` + `seq_next`). The lane claims `emit_seq` at dequeue under
  `lane->mu` (arrival order) and wraps `run_result`+`run_finished` in a lane-scoped
  `EmitTurn`. Compute stays fully parallel; only emission serialises, per group.
  Dropped frames never get a seq (no gap); stop wakes every gate so it can't wedge.
- **Caveats** (arrival mode): (a) dropped-frame `run_result`s (`XI_SYS_DROPPED`) are
  emitted at the drop site, *not* through the gate, so they may interleave out of
  order with the inspected stream — the guarantee covers *inspected* runs. (b) At
  `cmd:stop` the gate releases every in-flight worker out of turn (so stop can't
  deadlock), so the final ~`max_parallel` emits per group can be unordered. Ordering
  holds during steady-state operation, not across the stop boundary.
- Regression: `qa_dispatch_groups` Test E — arrival group (max_parallel 3 +
  anti-correlated sleep) → 0 `run_id` inversions, while a completion control shows
  many (the workload genuinely scrambles).

### Later
- `XI_SYS_CRASHED` / `XI_SYS_TIMEOUT` synthesis; `na_reason` text; Pareto card.
- `part_id` / `recipe` / `defects` traceability fields; MES export; gateway
  consuming `run_result` directly.

## See also
- [`production-hmi.md`](./production-hmi.md) — verdict / yield / Pareto cards.
- [`dispatch-groups.md`](../internals/dispatch.md) — the overflow drop site that emits `XI_SYS_DROPPED`.
- [`guides/write-a-script.md`](../guides/write-a-script.md) — `VAR` vs `RESULT`.
