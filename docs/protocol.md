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
- `value` (any) — inline value, present for non-image kinds
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
{ "type": "event", "name": "script_reloaded", "data": { "path": "..." } }
{ "type": "event", "name": "isolation_dead", "data": { "instance": "cam0" } }
{ "type": "event", "name": "state_dropped", "data": { "old_schema": 1, "new_schema": 2 } }
```

`isolation_dead` fires once per instance the first time
`use_process` / `use_exchange` sees an isolated instance gone
permanently dead (worker respawn cap hit; subsequent calls would
return safe defaults silently). Pair with the `log` (level=error)
message that lands in the same beat. Reset by reopening the project
or removing/recreating the instance.

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

Stable set for v1. Arguments are listed under each entry.

### `ping`
`args: {}` → `data: { "pong": true, "ts": <unix_seconds> }`

### `version`
`args: {}` → `data: { "version": "0.1.0", "abi": 1, "commit": "abc123" }`

### `shutdown`
`args: {}` → `ok: true` then the backend closes the socket and exits.

### `compile_and_load`
`args: { "path": "C:/.../inspection.cpp" }`
→ `data: { "build_log": "...", "instances": [...], "params": [...] }`
Invokes the C++ compiler (see M5), loads the resulting .dll, runs any global
constructors (which populate the registries), and returns the new state.

### `unload_script`
`args: {}` → `ok: true`

### `run`
`args: { "frame_path": "..." (optional) }`
→ `data: { "run_id": <int>, "ms": <int> }`
followed by an asynchronous `vars` message and zero or more binary previews.

`frame_path` is plumbed to the script as `xi::current_frame_path()`
(see `docs/guides/writing-a-script.md`). Empty / missing means the
script gets an empty string. Combine with `xi::imread()` to load a
file frame on demand without a custom source plugin.

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

Process-isolated instances and SHM-backed handles are NOT counted
here — they live in the worker's local pool / SHM region.

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

On compile failure the rsp is `ok: false` with `error` set to the
short build error and the **same `{plugin, diagnostics, reattached}`
structure attached as `data`** so live-tune loops can inspect
`diagnostics` programmatically. The Python SDK surfaces this as
`ProtocolError.data` (see `xinsp2.client.ProtocolError`).

### `open_project_warnings` *(planned, not yet wired)*

The plugin manager records non-fatal load issues (missing/broken
`instance.json`, factory throws) in `last_open_warnings_`, but no WS
handler currently exposes them. Calling this command returns a generic
"unknown command" error today. Until it's wired, fall back to
the `log` channel — those warnings are also emitted as `level: warn`
log messages during `open_project`.

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

## Connection lifecycle

1. Client connects to `ws://host:PORT/`.
2. Backend sends a welcome `event` with `name: "hello"` and `data: { "version": "...", "abi": 1 }`.
3. Client sends `cmd: version` to double-check, then `cmd: load_project` if it has one.
4. Client drives `compile_and_load` → `run` cycles.
5. Either side closes the socket to end the session; backend on `cmd: shutdown` also exits its process.

No heartbeat ping/pong beyond what WebSocket itself provides. Single-client
v1 does not need session resumption.
