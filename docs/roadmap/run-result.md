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
    XI_SYS_TIMEOUT    = -999004,   // (design sketch; never implemented — see note below)
    XI_SYS_NO_VERDICT = -999005,   // ran to completion but script set no RESULT
};
```

> **[2026-07-11] Watchdog wire delta (commit `93de38b`):** there is **no
> watchdog-produced result** on the wire. The cooperative soft-cancel layer was
> retired, so a frame that exceeds its budget but returns during the grace
> window emits a **normal trusted verdict**, and a genuinely wedged frame
> hard-exits the backend (`_Exit`) for the supervisor to respawn — it never
> emits a `no_verdict`/`watchdog_cancelled` (or `XI_SYS_TIMEOUT`) result.
> Also (commit `a293cfe`): a verdict set from an `xi::async` /
> `xi::parallel_for` worker now **reaches the run** instead of being silently
> dropped (which used to surface as a spurious `no_verdict`).

Consumer rule: `code <= -990000` → **system fail** (infrastructure, not a real
reject); `-990000 < code < 0` → **user NG**; `> 0` → **OK**; `0` → **NA**. So the
HMI can show a process reject (ng) differently from "the line dropped a frame"
(system), and yield math can exclude system fails or bucket them separately.

## The key rule: the framework fills the non-run cases

A **dropped** event never runs the script, so the script *cannot* emit its result —
**the dispatcher emits it** at the drop site (e.g. `enqueue_to_lane_` /
`enqueue_event_` overflow → `RESULT = XI_SYS_DROPPED`). Likewise the run loop
synthesizes `XI_SYS_CRASHED` (inspect threw) and
`XI_SYS_NO_VERDICT` (script ran but called no `RESULT`); there is no watchdog
case — a wedged frame ends in `_Exit`+respawn, never a result (see the
2026-07-11 note above). This is what guarantees
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

**Identity (additive — shipped; all OPTIONAL, omitted when empty so existing
consumers are unaffected; the numeric `code` is UNCHANGED for every case):**
- `trigger_id` — the run's 128-bit trigger id as a 32-char lowercase hex string
- `boot_id` — random 128-bit id (hex) generated ONCE per backend process at
  startup; stable for the process lifetime, so it groups a run of the same boot
- `station_id` — optional physical-station id from env `XINSP_STATION_ID`
  (omitted when unset)
- `inspection_id` — composite `"<station_id>/<boot_id>/<run_id>"` (present only
  when `run_id ≥ 0`; `station_id` segment may be empty)
- `schema` — result-record version; stable value `"xi.run-outcome/1"`
- `class` — outcome class string DERIVED from `code` (not a code change):
  `ok` (`code>0`) / `ng` (`-989999…-1`) / `na` (`0`) / `dropped` (`XI_SYS_DROPPED`)
  / `crashed` (caught inspect error) / `no_verdict` (ran, set no `RESULT`).
  On master the crashed case still rides `code=0`; see the BREAKING note below for
  the staged numeric-code semantics.
- `reason_code` — optional machine tag (e.g. `inspect_error`, `queue_full`);
  omitted for normal verdicts
- `script_generation` — a monotonically increasing integer identifying the
  **active loaded script version** (which compiled script DLL produced this
  result). Starts at `0` (no script ever loaded — the field is then omitted) and
  is bumped **exactly** at the hot-reload swap point where a freshly-compiled DLL
  becomes the one `inspect` calls. A **failed compile** or a failed load leaves
  it UNCHANGED — the last-good script stays active at its existing generation, so
  a consumer can tell that results are still coming from the previous DLL even
  though the editor may already show newer source. The value is **snapshotted at
  run start** (under the same lock as the script handle), so an in-flight run
  that began under generation `N` reports `N` even if a swap to `N+1` lands
  mid-run. Omitted on the drop path (no run ran) and when `0`/unknown.

**Optional (line / MES / traceability — add later):**
- `part_id` / `serial` — physical part (barcode)
- `recipe` / `program_version` — which inspection program version judged it
- `defects: [{ code, where }]` — for NG: failed features + location (feeds overlays)

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

## BREAKING (staged, not yet on master) — numeric-code semantics

> **This section documents wire semantics that live on branch
> `fix/extreview-c2-crash-break` only.** It changes the numeric `code` for two
> framework-generated cases and is intentionally held off master for a coordinated
> cutover with the app-dev team. On master these cases still emit `code=0` and are
> distinguished only by the additive `class`/`reason_code` fields.

Two non-verdict cases stop masquerading as NA (`0`) on the numeric channel; they
now emit their reserved system codes, so the numeric `code` and the derived
`class` agree:

| Case | code on master | code on this branch | class | reason_code |
|---|---|---|---|---|
| Caught inspect error (inspect threw/crashed) | `0` (NA) | `XI_SYS_CRASHED` = **-999002** | `crashed` | `inspect_error` |
| Ran to completion but script set no `RESULT` | `0` (NA) | `XI_SYS_NO_VERDICT` = **-999005** | `no_verdict` | *(none)* |

Notes for the cutover:
- **Actual enum values** (source of truth is `backend/src/service_main.cpp`):
  `XI_SYS_DROPPED = -999001`, `XI_SYS_CRASHED = -999002`, `XI_SYS_NO_VERDICT =
  -999005`. (The design sketch above uses illustrative values that predate the
  implementation; the source enum is authoritative.)
- A script that **explicitly** sets a verdict — including a legitimate NA (`0`) via
  `xi::result(0, …)` — is passed through unchanged. Only the *absence* of a verdict
  after a successful run now yields `XI_SYS_NO_VERDICT`.
- Both codes are in the reserved system band (`≤ -990000`), so consumers already
  treating that band as "system fail, not a real reject" need no change to bucket
  them correctly; only consumers that special-cased `code==0` to mean "crashed" or
  "no verdict" (which was ambiguous) must switch to the explicit codes / `class`.

## See also
- [`production-hmi.md`](./production-hmi.md) — verdict / yield / Pareto cards.
- [`dispatch-groups.md`](../internals/dispatch.md) — the overflow drop site that emits `XI_SYS_DROPPED`.
- [`guides/write-a-script.md`](../guides/write-a-script.md) — `VAR` vs `RESULT`.
