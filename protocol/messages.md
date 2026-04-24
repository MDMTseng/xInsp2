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
```

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

### `list_instances`
`args: {}` → triggers an `instances` message.

### `set_param`
`args: { "name": "sigma", "value": 3.5 }` → `ok: true`

### `set_instance_def`
`args: { "name": "cam0", "def": { ... } }` → `ok: true`

### `exchange_instance`
Generic passthrough to an instance's `exchange()` method — used by plugin
UIs that ship their own command vocabulary.
`args: { "name": "cam0", "cmd": { ... } }` → `data: <whatever the plugin returns>`

### `save_project` / `load_project`
`args: { "path": "project.json" }` → `ok: true`

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
