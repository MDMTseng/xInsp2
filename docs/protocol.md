# xInsp2 WebSocket Protocol

Single-client WebSocket protocol between the `xinsp-backend.exe` service and
its clients (VS Code extension, browser, CLI, test harness).

- **Framing**: WebSocket does framing. One message = one WS frame.
- **Text frames**: UTF-8 JSON objects. Every JSON message has a `type` field.
- **Binary frames**: image previews only. Layout defined below.
- **Versioning**: every `cmd` and `rsp` carries no explicit version — breaking
  schema changes bump the server's `version` string (returned by `cmd: version`).
  Clients check on connect and fail fast on mismatch.

---

## Text message types

Exactly six top-level `type` values. All JSON messages look like:

```json
{ "type": "<one of cmd|rsp|vars|instances|log|event>", ...fields... }
```

### `cmd` — client to backend

```json
{
  "type": "cmd",
  "id":   42,
  "name": "ping",
  "args": { "any": "json" }
}
```

| Field | Type   | Required | Purpose                                    |
|-------|--------|----------|--------------------------------------------|
| id    | int    | yes      | Correlation id. Echoed in the matching rsp.|
| name  | string | yes      | Command name, see command list below.     |
| args  | object | no       | Command-specific arguments. Default `{}`. |

### `rsp` — backend to client

```json
{ "type": "rsp", "id": 42, "ok": true,  "data": { ... } }
{ "type": "rsp", "id": 42, "ok": false, "error": "description" }
```

`data` is present only when `ok` is true. `error` is present only when `ok` is
false. Both are optional on failure too (an error with no message is legal
but discouraged).

### `vars` — backend to client

Snapshot of a `ValueStore` after one `inspect()` call.

```json
{
  "type": "vars",
  "run_id": 17,
  "items": [
    { "name": "gray",    "kind": "image",   "gid": 100, "raw": false },
    { "name": "blurred", "kind": "image",   "gid": 101, "raw": false },
    { "name": "count",   "kind": "number",  "value": 42 },
    { "name": "label",   "kind": "string",  "value": "ok" },
    { "name": "flag",    "kind": "boolean", "value": true },
    { "name": "report",  "kind": "json",    "value": { "pass": true } }
  ]
}
```

| Field  | Type   | Notes                                                  |
|--------|--------|--------------------------------------------------------|
| run_id | int    | Monotonic run counter. Same for all items in one run.  |
| items  | array  | In declaration order (as VAR macros execute).          |

Per-item fields:

- `name` (string, required)
- `kind` (string, required) — one of `image`, `number`, `boolean`, `string`, `json`, `custom`
- `value` (any) — inline value, present for non-image kinds. A non-finite
  `number` (NaN / ±Inf) is emitted as the JSON **string** `"NaN"` / `"Infinity"`
  / `"-Infinity"` (JSON has no non-finite numbers; emitting a bare `nan` would be
  invalid JSON and drop the whole frame). The same sentinel convention applies to
  non-finite doubles in a `Record` — they round-trip back to the non-finite value
  via `get_double`/`as_double` instead of silently reading as `0.0`.
- `gid` (int) — present for `image` kind; matches a subsequent binary preview frame
- `raw` (bool) — `true` if the image is transmitted uncompressed (BMP), `false` for JPEG

### `instances` — backend to client

Current state of the `InstanceRegistry` and `ParamRegistry` after a load or
on explicit request.

```json
{
  "type": "instances",
  "instances": [
    { "name": "cam0",          "plugin": "AravisCamera", "def": { ... } },
    { "name": "part_template", "plugin": "ShapeModel",   "def": { ... } }
  ],
  "params": [
    { "name": "sigma", "type": "float", "value": 3.5, "min": 0.1, "max": 10.0 },
    { "name": "low",   "type": "int",   "value": 60,  "min": 0,   "max": 255 }
  ]
}
```

### `log` — backend to client

Stdout / stderr from the backend and from user scripts.

```json
{ "type": "log", "level": "info", "msg": "compile ok", "ts": 1700000000.123 }
```

`level` is one of `debug|info|warn|error`.

### `event` — backend to client

Out-of-band notifications that don't fit the above.

```json
{ "type": "event", "name": "run_started", "data": { "run_id": 17 } }
{ "type": "event", "name": "run_finished", "data": { "run_id": 17, "ms": 42 } }
{ "type": "event", "name": "run_error", "data": { "run_id": 17, "what": "..." } }
{ "type": "event", "name": "run_result", "data": { "code": -2, "msg": "edge chip", "run_id": 17, "ms": 42, "source": "cam0", "group": "high" } }
{ "type": "event", "name": "script_reloaded", "data": { "path": "..." } }
{ "type": "event", "name": "state_dropped", "data": { "old_schema": 1, "new_schema": 2 } }
{ "type": "event", "name": "compile_started", "data": { "path": "..." } }
{ "type": "event", "name": "compile_finished", "data": { "path": "...", "ok": true } }
```

`run_started` / `run_finished` bracket every `cmd:run` and every
inspect dispatched by continuous mode. `run_started.data` carries
`{run_id}`; `run_finished.data` carries `{run_id, ms}` (the inspect
wall-clock duration). `run_error.data` is `{run_id, what}` and fires
INSTEAD of `run_finished` when the inspect throws (C++ exception or
SEH). Drivers waiting for run completion should listen for
`run_finished` OR `run_error` — exactly one fires per run.

`run_result` carries the run's **one verdict** (script-set via `xi::result`, or
`0` = NA if unset): `{code, msg, run_id, ms[, source, group]}`. Code convention:
`>0` ok-class, `0` NA, `-1…` ng-class, `<= -990000` framework system-fail enum.
It fires once per run (after vars, before `run_finished`) **and** once per
**dropped** trigger (queue overflow → `code: -999001` `XI_SYS_DROPPED`, with no
`run_id`/`ms`) — so a consumer sees one Result per trigger with no gaps. See
[`design/run-result.md`](./design/run-result.md).

`compile_started` / `compile_finished` bracket the `cmd:compile_and_load`
operation. `compile_started` fires immediately before cl.exe is invoked
(useful so drivers can show "compiling..." UI without parsing log
lines); `compile_finished` fires immediately after with the same
`path` plus an `ok` boolean so a listener that missed the rsp can
still tell success from failure. Cold compiles take 4+ s on this
project and dominate the WS quiet window — without these events the
connection looks hung.

`state_dropped` fires after `cmd:compile_and_load` when the new
script DLL declares a different `xi_script_state_schema_version()`
than the old one (and both are non-zero). The persisted `xi::state()`
JSON would default-fill into a different shape, so the backend drops
it and the new script runs with empty state. Register from user
script code:

```cpp
XI_STATE_SCHEMA(2);    // file-scope macro at the top of inspect.cpp
```

(Earlier docs suggested `#define XI_STATE_SCHEMA_VERSION 2` before
including `<xi/xi.hpp>` — that didn't work because
`xi_script_support.hpp` is force-included via `cl.exe /FI` before
the user TU is parsed, so the user's `#define` arrived too late.
`XI_STATE_SCHEMA(N)` declares a runtime static initialiser, which
runs at DLL load and wins.)

---

## Binary frame layout — image preview

One WebSocket binary frame per image variable, sent after the `vars` message
that introduces it.

```
offset  size  field
  0     4B    gid        (uint32, big-endian)  — matches vars.items[*].gid
  4     4B    codec      (uint32, big-endian)  — 0=JPEG, 1=BMP, 2=PNG
  8     4B    width      (uint32, big-endian)
 12     4B    height     (uint32, big-endian)
 16     4B    channels   (uint32, big-endian)  — 1 | 3 | 4
 20     N     payload    — codec-dependent bytes
```

Total header = 20 bytes. Clients read the 20-byte header, then consume the
remainder of the frame as the payload.

Rationale for including width/height/channels in the header: JPEG decoders
on the UI side need dimensions up front for layout, and embedding them lets
the client allocate image buffers before decoding. BMP already has this
metadata in-band; for JPEG we want it out-of-band for speed.

---

## Commands

The backend implements ~50 commands. The core commands are documented in detail
below; additional commands are listed at the end of this section. Arguments are
listed under each entry.

### `ping`
`args: {}` → `data: { "pong": true, "ts": <unix_seconds> }`

### `version`
`args: {}` → `data: { "version": "0.1.0", "abi": 1, "commit": "abc123" }`

`abi` here is the **WS protocol** version (currently 1) — distinct from the C
plugin-ABI struct version `XI_ABI_VERSION` (2, see `reference/host_api.md`).

### `shutdown`
`args: {}` → `ok: true` then the backend closes the socket and exits.

### `compile_and_load`
`args: { "path": "C:/.../inspect.cpp" }`
→ `data: { "dll": "C:/.../inspect_v3.dll",
           "diagnostics": [...],
           "resumed_continuous": true (optional) }`

Invokes the C++ compiler (see M5), loads the resulting .dll, runs any global
constructors (which populate the registries). On compile failure the rsp
is `ok: false` with `error: "compile failed"` and `data.diagnostics` carrying
the structured cl.exe error array. **A failure is non-destructive**: the new DLL
is loaded into a temporary and only swapped in on success, so a compile error —
or a DLL that compiles but fails to load — leaves the previously-working script
(and the client's subscriptions / history) intact rather than wedging to a null
script.

A `path` ending in `.dll` is loaded directly as a prebuilt AOT script DLL (no
compile step); any other path is compiled. Both are **contained to the open
project folder**: a prebuilt `.dll` must resolve inside it (an absolute/UNC
out-of-tree path is refused — loading it would run arbitrary DllMain / static-
init code in-process), and every path / link-lib / toolchain string sourced from
`project.json` that reaches the cl.exe command line is rejected if it contains
shell metacharacters or a double-quote (command-injection guard).

Hot-reload semantics: across the call, the backend (a) saves and restores
`xi::state()` JSON (drops it on schema mismatch via `state_dropped`
event), (b) replays every `cmd:set_param` value the user pushed into
the previous DLL into the new one, (c) if `cmd:start` was active,
captures the fps + auto-resumes a fresh worker after the new DLL is
ready (rsp gets `"resumed_continuous": true`). The cl.exe rebuild gap
is unavoidable (~3-5 s cold) but the run continues afterwards. A reload that
lands while an inspect is still in flight (e.g. a detached `cmd:run`) is safe:
the old script DLL stays mapped (refcounted) until that inspect returns, so it
never executes from an unloaded module.

### `unload_script`
`args: {}` → `ok: true`. Also clears the param replay cache.

### `run`
`args: { "frame_path": "..." (optional) }`
→ `data: { "run_id": <int>, "ms": <int> }`
followed by an asynchronous `vars` message and zero or more binary previews.

`cmd:run` is the **deterministic single-shot** path (UI "Run", step-through). It
is rejected while continuous mode is active (`"cannot run while continuous mode
is active"`) and rapid runs are serialized so their `vars`/history arrive in
`run_id` order. Burst/throughput parallelism is the continuous-mode dispatch
pool's job (`parallelism.dispatch_threads` + the trigger bus / fps) — `cmd:run`
does not fan out.

`frame_path` is plumbed to the script as `xi::current_frame_path()`
(see `docs/guides/writing-a-script.md`). Empty / missing means the
script gets an empty string. Combine with `xi::imread()` to load a
file frame on demand without a custom source plugin.

### `start` / `stop`
`start args: { "fps": int (default 10) }` → `data: { "started": true,
"dispatch_threads": int }` (the int reflects the project's
`parallelism.dispatch_threads`, default 1; included so callers can
verify the pool size that just came up).
On already-running: `data: { "already": true }`.
**Continuous mode has two drivers — don't conflate them.** The real driver is
**triggers**: image sources `emit_trigger()` and the bus/lanes run `inspect()` per
frame (a run with no source/trigger is meaningless). `fps > 0` additionally runs a
**synthetic timer tick** — an *empty* trigger every `1000/fps` ms — purely so a
**source-less** script still ticks (dev edit→run loop, the no-camera HMI demo);
`xi::current_trigger()` is inactive for those ticks. **`fps <= 0` = trigger-only**:
lanes spawn + sources route, **no timer** — what any source-driven project should
use (`--autostart-fps=-1` headlessly). For *meaningful* periodic runs, write a
source plugin that emits on a timer rather than relying on the empty tick.

`stop args: {}` → `data: { "stopped": true }`.

`set_timer_fps args: { "fps": int }` → `data: { "fps", "interval_ms" }`. Retunes the
synthetic-tick rate **live** while continuous mode runs; `fps <= 0` = trigger-only
(no ticks, sources drive). `set_process_priority args: { "class": "high"|"above"|
"normal"|"below"|"realtime" }` → sets the backend's OS process priority live (Win).
Both mirror `project.json` `"runtime": { "timer_fps", "process_priority" }`, which
the backend applies on `open_project`; the VS Code Project Settings UI sends these
commands on change and persists to `runtime`.

`get_dashboard args: { "name"?: string }` → `data: { found, name, dashboard }`.
Serves the project's HMI dashboard: `<project>/dashboard.json` (or
`dashboard.<name>.json` when `name` is given), embedded verbatim. So the HMI only
ever needs the BE's WS URL — it asks the BE for its dashboard + all data, with no
filesystem coupling. `name` is token-guarded (no path traversal); a missing file →
`found:false` (the HMI then keeps its static fallback).

Continuous mode runs `parallelism.dispatch_threads` worker threads inside
the backend (default 1; see `docs/guides/writing-a-script.md` → Parallel
dispatch for the pool, per-instance reentrancy, watchdog, and `result_order`).
Each tick comes from one of two sources:

- The trigger bus dispatches one inspect call per complete trigger
  (see `instance-model.md` trigger sections).
- A wall-clock timer at the requested fps fires a fallback dispatch
  even when no trigger is queued. Scripts that read trigger images
  must guard `xi::current_trigger().is_active()` because timer-only
  ticks have no trigger attached.

`vars` messages are emitted on each dispatch, same shape as for
`cmd:run`. There is no per-frame rsp; the only ack for `start` is the
initial one.

`cmd:start` **resets** the per-run dispatch counters used by
[`dispatch_stats`](#dispatch_stats) — `dropped_oldest`,
`dropped_newest`, and `queue_depth_high_watermark` all zero. Drivers
that snapshot `dispatch_stats` before *and* after a `cmd:start` and
subtract will get nonsense across run boundaries. Either record only
the AFTER snapshot, or treat the field values as scoped to the most
recent cmd:start window.

### `dispatch_stats`
`args: {}` → `data: { ... }`. Snapshot of the dispatch queue's
health since the most recent `cmd:start`:

| Field | Meaning |
|---|---|
| `queue_depth_now` | current queue size at snapshot time |
| `queue_depth_cap` | configured `project.parallelism.queue_depth` |
| `queue_depth_high_watermark` | peak queue depth observed since last `cmd:start` |
| `overflow` | configured policy: `drop_oldest` / `drop_newest` / `block` |
| `dispatch_threads` | configured `project.parallelism.dispatch_threads` |
| `dropped_oldest` | events dropped under `drop_oldest` since last `cmd:start` |
| `dropped_newest` | events dropped under `drop_newest` since last `cmd:start` |

The three counter fields (`dropped_*`, `queue_depth_high_watermark`)
are reset on every `cmd:start`. Sample after stop for the
end-of-run total; do not subtract a pre-start snapshot.

### `list_instances`
`args: {}` → triggers an `instances` message.

### `list_plugins`
`args: {}` → `data:` JSON array, one entry per registered plugin:

```json
[
  { "name": "blob_analysis", "description": "...", "folder": "...",
    "has_ui": true, "loaded": true, "origin": "global",
    "cert": { "present": true, "valid": true, ... },
    "manifest": { "params": [...], "inputs": [...], "outputs": [...] } }
]
```

`manifest` is present only if the plugin's `plugin.json` defines a
top-level `manifest` block (free-form; see
`docs/reference/plugin-abi.md`). Backend passes it through verbatim —
older plugins simply omit the field.

### `recent_errors`

Returns the last 64 errors captured across the three asynchronous
error channels (`rsp.error`, `log` level=error, `event` errors).
Lets a scripted client correlate "the cmd I just sent" with any
side-channel errors that landed around the same time — the WS spec
doesn't carry `cmd_id` / `run_id` on async events / logs yet, so
this ring is the workaround.

`args: { "since_ms": <int> (optional) }` — return only entries with
`ts_ms >= since_ms`. Use the `ts_ms` of the last-known error from a
previous poll to fetch incrementally.

### `toolchain_health`

`args: {}` → reports the C++ build toolchain for the open project. Each
component is resolved with priority **project override → environment variable →
built-in probe**, and the same resolved paths feed both the compiler and the
generated `c_cpp_properties.json` (so IntelliSense can't drift from the build).

```jsonc
{ "all_ok": true, "project": "<dir>", "components": [
  { "key": "include",   "label": "xi headers",      "ov_key": "include_dir",
    "env_var": "",            "path": "...", "source": "default",
    "exists": true, "optional": false, "ok": true,  "hint": "" },
  { "key": "opencv",    "label": "OpenCV",           "ov_key": "opencv_dir",
    "env_var": "OpenCV_DIR",  "path": "...", "source": "env",     "ok": true, ... },
  { "key": "turbojpeg", "label": "libjpeg-turbo",    "ov_key": "turbojpeg_root",
    "env_var": "TURBOJPEG_ROOT", "optional": true, ... },
  { "key": "ipp",       "label": "Intel IPP",        "ov_key": "ipp_root",
    "env_var": "IPP_ROOT",       "optional": true, ... },
  { "key": "vcvars",    "label": "MSVC (vcvars64)",  "ov_key": "vcvars",
    "env_var": "",            "path": ".../vcvars64.bat", ... }
]}
```

- `source`: `"override"` (pinned in `project.json`), `"env"`, `"default"` (built-in
  candidate), or `"none"` (unresolved).
- `ok`: an explicit override that doesn't resolve is always `false`; a **required**
  component (`include`/`opencv`/`vcvars`) must exist; an **optional** accelerator
  that's simply absent is `ok: true`.
- `hint`: human-readable fix when `ok` is false (or "optional, not installed").

### `set_toolchain_override`

`args: { "key": "include"|"opencv"|"turbojpeg"|"ipp"|"vcvars", "path": "<dir-or-file>" }`
→ pins (or, with an empty `path`, clears) one path in the canonical
`project.json` `"toolchain"` block:

```json
{ "toolchain": { "opencv_dir": "D:/libs/opencv/build",
                 "vcvars": "D:/VS/VC/Auxiliary/Build/vcvars64.bat" } }
```

The backend re-resolves the globals immediately, then replies `{ "applied": true,
"recompile_needed": true, "health": <toolchain_health> }` — the VS Code extension
refreshes `c_cpp_properties.json` from that health. The change takes effect on the next
`compile_and_load`. Surfaced in the editor under **Project Settings → C++
Toolchain** (see [`guides/install.md`](./guides/install.md) §5).

Reply (`data` is an array, newest last):

```json
[
  { "ts_ms": 1777300000123, "source": "rsp",
    "message": "compile_and_load: missing path", "cmd_id": 17 },
  { "ts_ms": 1777300000456, "source": "log",
    "message": "script crashed after 12ms: 0xC0000005 (...)",
    "run_id": 42 }
]
```

`cmd_id` / `run_id` are present only when the error site knew about
them. Migration of all error sites to the unified `emit_error_log` /
`send_rsp_err` helpers is incremental — older sites still emit the
log without recording, so the ring is best-effort coverage today.

A happy-path call returns `[]` — that's the "nothing to report"
answer, not an indication the ring is disabled. To see one in action,
trip a deliberate failure and re-poll:

```python
from xinsp2 import Client
with Client() as c:
    try: c.call("nonexistent_cmd")
    except Exception: pass
    print(c.recent_errors())
    # → [{'ts_ms': ..., 'source': 'rsp',
    #    'message': 'unknown command: nonexistent_cmd', 'cmd_id': N}]
```

### `image_pool_stats`

Per-owner ImagePool footprint. Each plugin instance and each loaded
script gets a unique `owner` id; allocations made on behalf of that
owner are tagged. Use this to spot leaks — a plugin / script whose
handle count keeps climbing across runs is holding pool entries it
should be releasing.

`args: {}` →

```json
{
  "total":      { "handles": 47, "bytes": 14745600 },
  "cumulative": { "total_created": 8210,
                   "high_water":    52,
                   "live_now":      47 },
  "by_owner": [
    { "owner": 1, "label": "script:inspect_v3.dll",
      "handles": 32, "bytes": 9830400 },
    { "owner": 2, "label": "instance:det (local_contrast_detector)",
      "handles": 15, "bytes": 4915200 },
    { "owner": 0, "label": "<host>",
      "handles": 0,  "bytes": 0 }
  ]
}
```

`total` and `by_owner` are **live snapshots** — they reflect what's
in the pool right now. Between runs (after `emit_vars_and_previews`
finishes releasing the run's VAR images), they typically drop to
zero, which can be confusing. `cumulative` solves that:

- `total_created` — every `image_create` since backend startup.
- `high_water`   — peak `live_now` ever observed.
- `live_now`     — same as `total.handles` (alias for clarity).

For "did this script ever allocate?" / "is the peak growing across
runs?" use `cumulative`. For "what's holding memory right now?"
use `by_owner`.

`label` is human-readable (`script:<dll>` / `instance:<name> (<plugin>)` /
`<host>`); the `(orphan)` suffix appears when an owner_id can't be
matched to a live instance — those are the sweep candidates the
ledger missed (rare, indicates a logic gap).

Backend automatically `release_all_for(owner)` sweeps:
- on `CAbiInstanceAdapter` destruction (instance destroyed)
- on `unload_script` (compile_and_load reload)

All instances and scripts run in-process, so every handle is counted
here — there is a single host ImagePool.

### `set_param`
`args: { "name": "sigma", "value": 3.5 }` → `ok: true`

### `set_instance_def`
`args: { "name": "cam0", "def": { ... } }` → `ok: true`

### `exchange_instance`
Generic passthrough to an instance's `exchange()` method — used by plugin
UIs that ship their own command vocabulary.
`args: { "name": "cam0", "cmd": { ... } }` → `data: <whatever the plugin returns>`

### `save_project` / `load_project` / `open_project`
`args: { "path": "project.json" }` → `ok: true`

`load_project` / `open_project` reattach instances and restore Param
values, but **do NOT recompile the inspection script** — call
`compile_and_load` separately afterwards. Cold opens of a project with
N project-local plugins compile each plugin under `cl.exe` and can
take 30–120 s; clients should pass a long timeout for this command.

**IntelliSense config.** The `.vscode/c_cpp_properties.json` that lets the
Microsoft C/C++ extension resolve `<xi/...>` and OpenCV is written by the **VS
Code extension** (NOT the backend core): on open it reads the resolved compile
paths from the backend via `toolchain_health` and mirrors the exact include set,
C++ standard, and force-included support header into the *canonical* project
folder, plus a `.vscode/extensions.json` recommending `ms-vscode.cpptools`. The
generated file is stamped `"_generated_by": "xinsp2"` and only files carrying that
stamp are overwritten (delete the stamp to take manual ownership). Both files
embed machine-specific absolute paths and are git-ignored under `examples/`.

**External script dependencies.** `project.json` may carry two optional arrays
that feed the script compile: `"include_dirs"` (extra `cl /I` paths) and
`"link_libs"` (extra import libs to link). Relative entries resolve against the
project folder. The matching dependency DLL is found at runtime because the
backend adds the project folder to the process DLL search path. See
[`guides/writing-a-script.md`](./guides/writing-a-script.md) → "Using an external
library / DLL" and [`examples/script_external_dll`](../examples/script_external_dll).

**Working-copy mode.** `open_project` accepts `"working_copy": true`: the
backend then operates on a `<project>/.xinsp_work` scratch copy (resume if
present, else seed from the canonical project), so edits are transactional and
crash-durable. The reply data then carries `"working_copy": true`,
`"canonical_path"`, and `"working_dir"`. Two paired commands:
- `commit_working_copy` `args: {}` → mirror the scratch back onto the canonical
  project (add + overwrite + delete-removed). Reply `{ "committed": true, "canonical": "<dir>" }`.
- `discard_working_copy` `args: {}` → delete the scratch, re-seed from canonical,
  reopen. Reply is the project JSON (same shape as `open_project`).

See [`guides/project-working-copy.md`](./guides/project-working-copy.md). The
headless `--working-copy` flag opts autostart into the same mode (so an FE
respawn after a crash resumes the scratch).

### `recompile_project_plugin`

Hot-rebuilds a single project-local plugin. The extension's file watcher
calls this when the user edits plugin source; the Python SDK exposes it
as `c.recompile_project_plugin(name)`. On success, instances of that
plugin are re-instantiated with their previous defs intact; on failure
the old DLL stays loaded so a running inspection isn't disrupted.

`args: { "plugin": "<plugin_name>" }`

Reply data:
```json
{
  "plugin": "local_contrast_detector",
  "diagnostics": [
    { "file": "...", "line": 42, "col": 5,
      "severity": "error", "code": "C2065", "message": "..." }
  ],
  "reattached": ["det0", "det1"]
}
```

This is the linchpin command for the live-tune workflow: edit a
project plugin's source, hit save, recompile, watch instances pop back
with their state intact and the next `run` use the new code.

### `open_project_warnings`

Returns the list of non-fatal load issues from the most recent
`open_project` call: missing/broken `instance.json`, factory throws,
unknown config keys, type mismatches, out-of-range values, enum
violations.

```json
{ "type": "cmd", "id": 8, "name": "open_project_warnings" }
```

Reply:

```json
{ "type": "rsp", "id": 8, "ok": true,
  "data": { "warnings": [
    { "instance": "cam0", "plugin": "burst_source", "reason": "..." }
  ] } }
```

`warnings` is empty for a clean open. Each entry has `instance`,
optional `plugin`, and a human-readable `reason`. The same warnings
are also emitted on the `log` channel as `level: warn` during
`open_project` so a UI listener can surface them in real time.

> Earlier versions of this doc said "planned, not yet wired" — the
> handler has been wired since the FL r6 P2-3 fix (PR #25). Doc
> updated 2026-05-10.

### `history` / `set_history_depth`

Backend keeps a ring buffer of the last N vars snapshots so a client
can scrub backward through recent runs without re-executing. Default
depth is 50.

```json
{ "type": "cmd", "id": 9, "name": "history", "args": { "count": 5 } }
```

Reply (newest first):

```json
{ "depth": 50, "size": 12,
  "runs": [
    { "run_id": 12, "ts_ms": 1777..., "vars": [ ... ] },
    { "run_id": 11, "ts_ms": 1777..., "vars": [ ... ] },
    ...
  ] }
```

Optional `since_run_id`: stop once a run with that id-or-older is hit
(useful to incrementally pull only new entries).

`cmd: set_history_depth { depth: N }` resizes the ring; entries beyond
the new cap are dropped immediately. Bounded to [0, 10000].

### `compare_variants`

Run the loaded script once under each of two "variants" (sets of
`Param` values + instance defs), back-to-back, and return both vars
snapshots. Client-side code diffs to answer "what does sigma=3 vs
sigma=4 look like for THIS frame?" without juggling two backends.

```json
{ "type": "cmd", "id": 7, "name": "compare_variants",
  "args": {
    "a": {
      "params":    [ { "name": "sigma", "value": 3 } ],
      "instances": [ { "name": "det0",  "def": { "threshold": 120 } } ]
    },
    "b": {
      "params":    [ { "name": "sigma", "value": 4 } ],
      "instances": [ { "name": "det0",  "def": { "threshold": 150 } } ]
    }
  } }
```

Reply:

```json
{ "a": { "vars": [ ... snapshot ... ] },
  "b": { "vars": [ ... snapshot ... ] } }
```

After the call the script is left in **variant B**'s state — follow
with your own `set_param` / `load_project` if you need to restore.

### `resume`

Releases a script that's blocked inside `xi::breakpoint("label")` (S3).
When the script hits a breakpoint the backend emits:

```json
{ "type": "event", "name": "breakpoint", "data": { "label": "after_gray" } }
```

The client inspects the last `vars` message and whatever else it wants,
then sends `cmd: resume` to let the script continue. Response:

```json
{ "resumed": true, "label": "after_gray" }
```

Calling `resume` when nothing is paused replies `{ "resumed": false }`.
Breakpoints block the worker thread running `inspect()`, so they only
take effect during continuous mode (`cmd: start`). A blocked breakpoint
is auto-released when the worker is joined for `cmd: stop` /
`cmd: compile_and_load`, so neither of those can deadlock.

### `subscribe` / `unsubscribe`

Controls which VAR-image previews are JPEG-encoded and streamed as
binary frames after each `run`. Defaults to "send everything"
(back-compat); set an explicit list to avoid wasting CPU + bandwidth on
images the viewer isn't showing.

- `cmd: subscribe`  `args: { "names": ["gray", "edges"] }` — stream
  preview frames only for vars in the list. Repeatable; each call
  REPLACES the list. Pass `{ "all": true }` to re-enable send-all.
- `cmd: unsubscribe` — empty the list. No `preview` binary frames emitted
  after subsequent runs until `subscribe` is called again. `vars`
  (metadata) is still sent either way.

Example:
```json
{ "type": "cmd", "id": 5, "name": "subscribe", "args": { "names": ["gray"] } }
```

Large-image inspections (20 MP frames at ~1 MB JPEG each) benefit
significantly — a 5-var pipeline with a 1-var subscription uses
~80% less upstream bandwidth.

---

## Error handling

- Malformed JSON: backend sends `{ "type": "log", "level": "error", "msg": "..." }` and keeps the connection open.
- Unknown command name: `rsp` with `ok: false, error: "unknown command: xyz"`.
- Exception inside a command handler: `rsp` with `ok: false, error: <what()>`.
- Script runtime exception: emitted as an `event` with `name: "run_error"` and `data: { "what": "..." }`. The `rsp` for the `run` command still returns `ok: true` if the run started — failure is reported via the event channel so partial vars can still be delivered.

---

## Status channel

Components publish a short, sticky "what am I doing right now" string (distinct
from `VAR` per-inspection *values* and from the `log`/`recent_errors` *event
stream* — status is one last-value string per component, overwritten in place).

- A script calls `xi::status("waiting for trigger")` (include `<xi/xi_status.hpp>`).
  The host stores it under the key `@script`.
- A plugin calls `status("grabbing")` (the `xi::Plugin::status` helper, or the
  `host_api->set_status(source, text)` C ABI). The host keys it by the instance
  name (e.g. `cam0`). ABI-additive — older plugins/hosts simply don't use it.
- `cmd:status` → `data: { "<source>": { "text": "...", "ts_ms": N, "seq": N }, ... }`
  — a snapshot of every component's latest status.
- `event: status` → `data: { "source": "...", "text": "...", "seq": N }` — pushed
  best-effort when a status changes.

**Delivery guarantee:** the backend *retains* the latest value (last-write-wins),
so a client should call `cmd:status` on **every (re)connect** to re-sync — that
snapshot-over-retained-state is what guarantees the latest status always arrives,
even across disconnects and backend respawns. The `status` event is only a
low-latency accelerator between snapshots (identical repeats are coalesced, so it
doesn't spam). The latest status is also mirrored into the crash breadcrumb, so it
appears as `last_status` in the crash report if the backend dies.

## Connection lifecycle

1. Client connects to `ws://host:PORT/`.
2. Backend sends a welcome `event` with `name: "hello"` and `data: { "version": "...", "commit": "<git-sha>", "abi": 1 }`.
3. Client sends `cmd: version` to double-check, then `cmd: load_project` if it has one.
4. Client drives `compile_and_load` → `run` cycles.
5. Either side closes the socket to end the session; backend on `cmd: shutdown` also exits its process.

No heartbeat ping/pong beyond what WebSocket itself provides. Single-client
v1 does not need session resumption.

### Single-client enforcement

Only one WS client may be connected at a time. While a client is
connected, the server still calls `accept()` on incoming connections
(so the OS SYN queue does not fill) but immediately responds with
`HTTP/1.1 503 Service Unavailable` and `X-Xi-Reason: single-client-busy`,
then closes the socket. Callers should treat 503 from the upgrade
endpoint as "another client owns the backend; retry after they
disconnect" rather than a backend health problem.

Prior to FL r7 this case was not handled — a 2nd connection's SYN
would sit in the kernel queue until Windows timed it out (~21 s),
which surfaced to the caller as a long stall. The `accept()`-and-
reject path replaces that with a fast, diagnosable error.

### Additional implemented commands

The following commands are implemented in `service_main.cpp` but not documented
in full detail above. One-line purpose per entry; args follow the same
`cmd`/`rsp` envelope.

#### Crash diagnostics

| Command | Purpose |
|---|---|
| `crash_reports` | Return the JSON crash reports written by previous fatal crashes (from `%TEMP%/xinsp2/crashdumps/*.json`), newest-first. |
| `clear_crash_reports` | Delete all crash JSON files from the dump directory. Reply: `{ "removed": N }`. |

#### Watchdog

| Command | Purpose |
|---|---|
| `set_watchdog_ms` `args: { "ms": N }` | Set the per-inspect wall-clock budget in ms (0 = disabled). Reply: `{ "ms": N, "trips": N }`. |
| `watchdog_status` | Current watchdog config and trip count. Reply: `{ "ms": N, "trips": N, "armed": bool }`. |

#### Pipeline graph capture

| Command | Purpose |
|---|---|
| `graph_capture` `args: { "enable": bool }` | Toggle dataflow edge recording (default off — no hot-path cost). Clears any prior recording on enable. Reply: `{ "capturing": bool }`. |
| `graph_snapshot` | Reconstruct dataflow edges from the recorded calls by image-handle identity (instance A produced handle H; instance B consumed H → A→B edge). Reply: `{ "capturing": bool, "ran": ["inst", ...], "edges": [{ "from": "A", "to": "B", "keys": ["mask"] }, ...] }`. The `ran` list is in call order; `keys` lists the output-image names that crossed the edge. |

#### Recording / replay

| Command | Purpose |
|---|---|
| `recording_start` `args: { "path": "...", "max_frames"?: N }` | Start recording trigger events (image handles + metadata) to a file for deterministic replay. |
| `recording_stop` | Stop an active recording. Reply includes `{ "path": "...", "frame_count": N }`. |
| `recording_status` | Recording state and frame count so far. |
| `recording_replay` `args: { "path": "...", "loop"?: bool }` | Replay a recording file into the trigger bus (or dispatch pool if continuous mode is active). |

#### Params and preview (out-of-band operations)

| Command | Purpose |
|---|---|
| `list_params` | List all registered `xi::Param` values (name, type, value, min/max if numeric). |
| `preview_instance` `args: { "name": "cam0", "timeout_ms"?: N }` | Grab one frame from an `ImageSource` instance and return it as a binary preview (no inspect). Useful for live camera aiming without running the full pipeline. |
| `process_instance` `args: { "name": "det0", "images": [...], "json"?: "..." }` | Call a plugin instance's `process()` directly (bypasses the script and the trigger bus). Input images are supplied as base64-encoded PNG/JPEG in the args; output images are returned the same way. Intended for unit-testing individual plugin instances from outside the script. |

#### Plugin management

| Command | Purpose |
|---|---|
| `rescan_plugins` | Rescan the global plugins directories and refresh manifests. Does not reload already-loaded plugin DLLs. |
| `load_plugin` `args: { "name": "...", "folder"?: "..." }` | Force-load (or reload) a specific plugin by name. Typically used after `rescan_plugins` found a new plugin. |
| `recertify_plugin` `args: { "name": "..." }` | Re-run the baseline certification tests for a plugin and update its cert file. |
| `export_project_plugin` `args: { "name": "..." }` | Package a compiled project-local plugin (DLL + manifest) for distribution; stamps `abi_version` in the exported `plugin.json`. |

#### Project and instance CRUD

| Command | Purpose |
|---|---|
| `create_project` `args: { "path": "...", "name": "..." }` | Create a new empty project folder with a stub `project.json` and `inspect.cpp`. |
| `close_project` | Tear down all instances, reset the trigger bus, stop recording. Does not unload script or plugin DLLs. |
| `create_instance` `args: { "plugin": "blob_analysis", "name": "det0" }` | Add a new instance to the open project (creates folder, calls `xi_plugin_create`, writes `instance.json`). |
| `remove_instance` `args: { "name": "det0", "purge"?: bool }` | Remove an instance from the registry and call `xi_plugin_destroy`. Default: keep the on-disk folder. Pass `"purge": true` to delete it. |
| `rename_instance` `args: { "old": "det0", "new": "detector" }` | Rename an instance (moves its folder, updates the registry). |
| `save_instance_config` `args: { "name": "det0" }` | Write the current `get_def()` output for one instance to `instance.json` without a full `save_project`. |
| `get_project` | Return the open project's `project.json` content and resolved metadata. |
| `get_plugin_ui` `args: { "name": "blob_analysis" }` | Return the plugin's `ui/index.html` content (for plugins with `has_ui: true`). Used by the VS Code extension to open the plugin webview. |

#### Trigger policy

| Command | Purpose |
|---|---|
| `set_trigger_policy` `args: { "policy": "any"\|"all_required"\|"leader_followers", ... }` | Update the project's trigger correlation policy live (without reloading the project). Changes take effect on the next trigger event. |

#### Dispatch stats

`dispatch_stats` is already documented above under `start` / `stop`.

#### History utilities

| Command | Purpose |
|---|---|
| `clear_history` | Empty the run-history ring immediately. Reply: `{ "cleared": N }`. |

---

## Backend command-line flags

`xinsp-backend.exe` is normally launched by a supervisor (the VS Code
extension, or `xinsp-fe.exe` on a line — see
[`design/fe-be-split.md`](./design/fe-be-split.md)). Key flags:

| Flag | Default | Purpose |
|---|---|---|
| `--port=N` | 7823 | WebSocket port |
| `--host=ADDR` | 127.0.0.1 | bind address (`0.0.0.0` for remote; pair with `--auth`) |
| `--auth=SECRET` | — | require `Bearer SECRET` in the WS handshake |
| `--plugins-dir=DIR` | — | extra plugin folder (repeatable) |
| `--watchdog=MS` | 0 (off) | terminate an inspect that exceeds MS ms |
| `--project=DIR` | — | **headless autostart**: `open_project` this folder at boot |
| `--script=PATH` | project.json's `script` | script to `compile_and_load` for `--project` |
| `--autostart-fps=N` | 0 (off) | with `--project`, `start` continuous mode at N fps; **N<0 = trigger-only** (start, no timer) |
| `--working-copy` | off | open via a `<project>/.xinsp_work` scratch (transactional; resumes on crash respawn) — see [working-copy guide](./guides/project-working-copy.md) |
| `--priority=CLASS` | (unchanged) | process priority class (Win): `high`/`above`/`normal`/`below`/`realtime`. `realtime` can starve the OS — use with care |
| `--aot` | off | prebuilt bundle: load existing plugin/script DLLs, **never invoke the compiler** (a `.dll` `script` path loads directly; plugins load the newest `build/*.dll`). See [`design/deployment.md`](./design/deployment.md) export bundle |

Performance notes: on Windows the backend raises the OS timer resolution to **1 ms**
(`timeBeginPeriod(1)`) at startup so sleeps / timer-tick fps / CV waits are tight,
and **logs a warning** if the total dispatch worker count (Σ per-group
`max_parallel`, or `dispatch_threads`) exceeds the core count (oversubscription →
context-switch thrash). See [`design/dispatch-groups.md`](./design/dispatch-groups.md)
for per-group `thread_priority` / `cpu_affinity`.

The `--project` autostart drives the same `open_project → compile_and_load →
start` commands a client would send, by synthesizing them internally after the
WS port binds. No client need ever connect (reply frames are no-ops with no
client), and the port stays open so an operator HMI / the extension can attach
live. This lets `xinsp-fe.exe` run a line at the process level without a C++ WS
client.
