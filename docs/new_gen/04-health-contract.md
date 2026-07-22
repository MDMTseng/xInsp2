# Health / State Contract

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Status** | Design + landed on `polaris/health-contract` (adoption-map item 10 / triage Bucket E) |
| **Implements** | [`01-xinsp3-architecture.md`](./01-xinsp3-architecture.md) change #2; [`03-adoption-map.md`](./03-adoption-map.md) item 10 |
| **Consolidates** | the one recurring ask of four reviews (01 #9, 02 I.9, 03, 05) — a canonical health/state contract owned by core |
| **Schema** | `xi.health/1` |

## Why this exists

Four independent reviews asked for the same thing: **one canonical read of "is
the system healthy, and if not, which part isn't."** Today a client has to
stitch that answer together from side channels — `dispatch_stats`
(`last_emit_age_ms`, queue drops), `get_state` (per-instance `created/active/faulted`),
the sticky `status` map, `recent_errors`, `metrics`, `watchdog_status`. Each was
added for its own reason; none is the whole picture, and every consumer infers
liveness differently (review 10's finding: the primary dev client ignores the
run-outcome lifecycle entirely).

This contract is the coherence primitive the triage flagged as the one deferred
item that fails the plugin test — it is genuinely **core-shaped**. It does not
add a new signal; it **subsumes the existing ones under one small, versioned
surface** the FE supervisor, HMI, extension, and headless runner can all read
the same way.

It is deliberately small: a state enum, a component list, and a set of reason
codes. It is **not** an observability platform. Metrics history, retention,
dashboards, and alerting policy remain plugin territory (see *Non-goals*).

## The top-level state machine

One process-wide state, owned by core:

```
   boot ──open──► project_loaded ──start──► running ⇄ degraded
    ▲   ◄─close──┘        ▲    ▲              │  │        │
    │                     │    └────drain─────┘  │        │
    └──────drain──────────┘   (draining) ◄───────┴────────┘
                                     │
   (any state) ─fatal─► fault ─exit+respawn─► boot (fresh process)
```

| State | Meaning |
|---|---|
| `boot` | Process is up; no project loaded. The initial state. |
| `project_loaded` | A project is open (instances constructed); dispatch is **not** running. |
| `running` | Continuous dispatch is live and every component is healthy. |
| `degraded` | Running, but at least one component has a **runtime** fault (a caught plugin crash, or a failed script). Frames still flow. |
| `draining` | A stop / lifecycle op is quiescing the dispatch pool (joining workers, draining queues). Transient. |
| `fault` | An unrecoverable condition (watchdog hard trip). The backend exits for the FE to respawn — the only exit from `fault` is a fresh process, which starts again at `boot`. |

### Legal transitions

Each row is one legal edge and the lifecycle event that drives it. `running ⇄
degraded` is **derived** (see below); every other edge is driven by an explicit
lifecycle verb.

| From | To | Trigger |
|---|---|---|
| `boot` | `project_loaded` | `open_project` / `load_project` succeeds |
| `project_loaded` | `project_loaded` | opening a different project (re-open) |
| `project_loaded` | `boot` | `close_project` |
| `project_loaded` | `running` | `start` (dispatch pool spawned) |
| `running` | `degraded` | *(derived)* the script fails **or** an instance takes a runtime fault while running |
| `degraded` | `running` | *(derived)* all runtime degradations clear |
| `running` / `degraded` | `draining` | `stop`, or a lifecycle op quiesces the pool |
| `draining` | `project_loaded` | drain complete, project still open (`stop`) |
| `draining` | `boot` | drain complete + project closed (`close_project` while running) |
| `running` / `degraded` | `project_loaded` | re-open drops the run without a distinct drain phase |
| any of the above | `fault` | watchdog hard trip / unrecoverable |
| `fault` | `boot` | **only** via process exit + FE respawn (a fresh process) |

The machine is **enforced but not fragile**: an undocumented transition is
applied anyway and logged to stderr (a dev signal that this table drifted from a
call site), never dropped or wedged. The documented table is the contract; the
implementation flags deviations rather than trusting memory.

### `running ⇄ degraded` is derived, and never touches the per-frame path

`degraded` is not set by a command. While the state is `running` or `degraded`,
core recomputes it from two **runtime** health inputs it owns:

- the **script** component is `failed`, or
- **any instance** carries a runtime fault (a caught `process()` crash).

Both inputs are updated only on rare events — a compile/load outcome, or the SEH
fault boundary catching a plugin crash. **A per-frame verdict never touches
health.** The hot path records a verdict and returns; only a *fault* (the caught
exception path, off the common path) marks a component degraded. Health updates
are an atomic state word plus a mutex-guarded map touched on lifecycle/fault
events — never per frame.

## The component model

A component is `{ kind, name, health, reason_code, since_ms }`:

- `health` ∈ `ok` | `degraded` | `failed`
  - `ok` — functioning.
  - `degraded` — a **runtime** fault: it ran and then faulted, but the system
    keeps it in service (this is the seed for adoption-map item 14's quarantine
    policy).
  - `failed` — it could not be brought into service (compile error, prepare/
    commit failure).
- `reason_code` — a short machine token (see below); empty when `ok`.
- `since_ms` — wall-clock ms timestamp when this health was entered (a consumer
  computes age as `now − since_ms`, matching the `status` channel's `ts_ms`).

### Component kinds and where their truth lives

The contract deliberately **does not duplicate** state that another core owner
already holds authoritatively — that hand-synced duplication is exactly the
failure mode the v3 architecture exists to kill (and the concrete bug the
instance-state map hit before it was moved into the PluginManager). Instead:

| Kind | Health source | Stored in the health registry? |
|---|---|---|
| `script` | compile/load outcome (`compile_and_load` / `unload_script`) | **yes** — one well-defined component, one choke point |
| `instance` | `InstState` (PluginManager, owns it) **overlaid** with the runtime-fault set | overlay **only** (the runtime-crash quarantine seed — new truth nobody else holds); base `active/faulted` is read from the PluginManager at query time |
| `group` | the dispatch lanes (`g_eng.lanes`) — a lane exists ⇒ `ok` | **no** — derived at query time |
| `source` | `TriggerBus::source_emit_ages_us()` — the existing emit-age tracking | **no** — derived at query time; **no second staleness tracker is built** |

So the registry stores exactly three things: the top-level state, the script's
health, and the runtime-fault overlay per instance. Everything else is read from
its existing owner when `get_health` is answered, so it cannot drift.

Instance health merges as: PluginManager `faulted` → `failed` /
`prepare_failed`; else a runtime-fault overlay entry → `degraded` /
`plugin_fault`; else `ok`. Sources report their `last_emit_age_ms` and are
always `ok` — **core does not invent a staleness threshold** (the expected rate
is source-specific; alerting is the consumer's call, unchanged from
`dispatch_stats`).

### Reason codes

| `reason_code` | Kind | Meaning |
|---|---|---|
| `plugin_fault` | instance | a caught `process()` / `exchange()` crash (SEH or C++ throw); the instance stays in service, `degraded` |
| `quarantined` | instance | `on_fault=refuse` (or a reinit that escalated) pulled the instance from service → `failed` (item 14) |
| `prepare_failed` | instance | `prepare` / `commit` / `set_def` failed → `failed` |
| `compile_error` | script | the last `compile_and_load` failed to build or load → `failed` |
| `watchdog_trip` | *(state)* | the fatal transition to `fault` |

The list is intentionally short and additive — a new code is a doc + a constant,
never a wire break.

## Wire surface

Two additions, both **additive** — existing clients are unaffected, and every
client already tolerates unknown events (review 10).

### Command `get_health`

Point-query snapshot, same role/shape family as `dispatch_stats` /
`image_pool_stats` / `metrics`. No arguments.

```json
{
  "schema": "xi.health/1",
  "state": "degraded",
  "since_ms": 1751430000123,
  "boot_id": "9f3c…",
  "station_id": "line3-cell2",
  "components": [
    { "kind": "script",   "name": "inspect.cpp", "health": "ok",       "reason_code": "",             "since_ms": 1751429990000 },
    { "kind": "instance", "name": "cam0",        "health": "degraded", "reason_code": "plugin_fault", "since_ms": 1751430000123, "crash_count": 3 },
    { "kind": "instance", "name": "sink0",       "health": "ok",       "reason_code": "",             "since_ms": 0 },
    { "kind": "group",    "name": "default",     "health": "ok",       "reason_code": "",             "since_ms": 0, "queue_now": 0, "running": 1, "dropped": 0 },
    { "kind": "source",   "name": "cam0",        "health": "ok",       "reason_code": "",             "since_ms": 0, "last_emit_age_ms": 41 }
  ]
}
```

`since_ms` at top level is when the current state was entered. `boot_id` /
`station_id` mirror the run-outcome identity slice already stamped on every
result. Instance / group / source entries carry a few extra derived fields
(`crash_count`, `queue_now`/`running`/`dropped`, `last_emit_age_ms`) so a
consumer need not also poll `get_state` / `dispatch_stats` for the common case —
the whole point is one read.

### Event `health_changed`

Emitted on every **top-level state transition** and on every **runtime-fault
overlay / script health change** (the caught-fault and compile-outcome paths). It
carries the current state, and — when a component changed — the component:

```json
{ "type": "event", "name": "health_changed",
  "data": { "schema": "xi.health/1", "state": "degraded", "since_ms": 1751430000123,
            "component": { "kind": "instance", "name": "cam0", "health": "degraded", "reason_code": "plugin_fault" },
            "ts_ms": 1751430000123 } }
```

```json
{ "type": "event", "name": "health_changed",
  "data": { "schema": "xi.health/1", "state": "running", "since_ms": 1751429990000, "ts_ms": 1751429990000 } }
```

A pure state transition (e.g. `start` → `running`) carries no `component`. The
event is a **low-latency accelerator**; `get_health` is the delivery guarantee
(a client re-pulls it on connect, exactly like `status`). Prepare-time instance
`failed` states are not pushed as `health_changed` (they surface in the next
`get_health` and already have `status` + `get_state`); the event stream is
reserved for state-machine transitions and runtime faults, keeping it low-noise.

## Non-goals (plugin territory, on purpose)

- **Metrics history / retention / dashboards** — `get_health` is a point-query
  snapshot; a plugin subscribes and stores.
- **Alerting / staleness thresholds** — core reports `last_emit_age_ms`; the
  consumer decides what "too stale" means (rate is source-specific).
- **Per-frame telemetry** — that is `metrics`; health never touches the hot path.
- **Recovery / quarantine policy** — the contract *marks* an instance
  `degraded` on a runtime fault; the reuse/re-init/refuse decision is
  adoption-map item 14, now landed on top of this overlay (see below).

## Quarantine policy (adoption-map item 14)

The overlay above is the *seed*; item 14 is the *decision* built on it. When a
plugin instance's `process()`/`exchange()` faults and the SEH boundary catches it
(`service_sinks.cpp` `use_process_inline_`), a per-instance **`on_fault`** policy
decides what happens next:

| `on_fault` | Behavior | Health | For |
|---|---|---|---|
| `reuse` *(default)* | fault logged; instance stays in service | `degraded` / `plugin_fault` | stateless operators — nothing changes vs. pre-item-14 |
| `reinit` | instance torn down + re-created + re-prepared from its last committed config before its next use, dropping in-flight state | `degraded` while faulted → clears on a clean rebuild | stateful plugins whose invariants may be corrupted mid-mutation |
| `refuse` | instance pulled from service; subsequent `process()` calls fail fast (rc `-3`) without entering plugin code | `failed` / `quarantined` | plugins where a wrong-but-not-crashing result is worse than no result |

**Declaration.** `on_fault` is a per-plugin default in `plugin.json`
(`"on_fault": "reinit"`) with a per-instance `instance.json` override — exactly
where the other dispatch knobs live (`reentrant`/`sink` in `plugin.json`,
`max_concurrency`/`group` in `instance.json`). Absent/unknown ⇒ `reuse`, so the
change is pre-1.0 compatible: nothing changes unless a plugin or instance declares
otherwise. See the plugin authoring guide and `ws-protocol.md`.

**Enforcement rides the existing lifecycle.** A `reinit` rebuild reuses the same
create → `set_def` steps as the plugin-reload path (`make_adapter_guarded_`), done
**in place** on the live adapter and **serialized by the instance's `CallScope`
gate** (so no `process()` runs concurrently on it and the `shared_ptr` other
dispatch workers hold stays valid) — it does not invent a second lifecycle. A
rebuild that fails keeps the old instance live (corrupt-but-runnable) and, after
`kReinitEscalateAfter` = **3** consecutive failures, escalates to `refuse` so a
persistently-unrebuildable instance stops thrashing the lifecycle every frame.

**Cost.** The check is off the non-fault path: the refuse fail-fast gate is a
single already-loaded atomic on the adapter (the natural home — the per-instance
gate already lives there), and the policy consultation itself happens only in the
rare caught-fault branch. A per-frame verdict never touches any of this.

**Re-enable** uses the existing operator surface — re-committing an instance's
config (`set_instance_def` / `commit_group`, which set it `Active`) lifts the
quarantine and clears the overlay. No new command; a reload/recompile also
produces a fresh, un-quarantined adapter.

## What this subsumes

| Existing signal | Relationship to the contract |
|---|---|
| `get_state` (per-instance `created/active/faulted` + `crash_count`) | promoted into the component list as instance health; `get_state` stays for a single-instance point query |
| `status` sticky map (`@compile`, `@script`, per-instance) | free-form operator text; the contract adds the **structured** health projection (enum + reason code) alongside it |
| `dispatch_stats` `last_emit_age_ms` / per-source ages | surfaced as `source` components (no second tracker) |
| `dispatch_stats` group queue/running/dropped | surfaced on `group` components |
| `watchdog_status` trips | the fatal path drives the `fault` state |

None of these are removed — the contract is the one coherent read *over* them,
and new consumers should prefer it.
