# xInsp2 WebSocket Protocol

Single-client WebSocket protocol between the `xinsp-backend.exe` service and
its clients (VS Code extension, browser, CLI, test harness).

- **Framing**: WebSocket does framing. One message = one WS frame.
- **Text frames**: UTF-8 JSON objects. Every JSON message has a `type` field.
- **Binary frames**: the only binary frames are **plugin-originated pushes**.
  A plugin calls the ABI v8 `emit_binary(data, len)` host service and the core
  forwards the bytes verbatim to connected clients via `send_binary` — the core
  is a dumb byte pipe; the frame format is the plugin's contract with its UI
  (the shipped `expose` plugin uses this to push one atomic `XEX1` frame per run —
  values + JPEG images — see *The `expose` plugin* below). This is distinct from
  the **removed** core image-preview binary frame (and the `vars` message and the
  old core `subscribe`/`unsubscribe` commands): script output (scalar values +
  images) now goes through the shipped `expose` plugin, not core transport. The
  removed shapes are described below, struck through, for reference only.
- **Versioning**: every `cmd` and `rsp` carries no explicit version. The
  server's `version` string (returned by `cmd: version` and in the `hello`
  event) is the intended breaking-change signal; the sibling `abi` field is a
  hardcoded constant `1` that has never been bumped. The protocol evolves
  **additive-only** (new fields like vars' `src`/`group`, new commands), so old
  clients ignore unknown fields and new clients tolerate missing ones; unknown
  commands reply `ok:false`. **There is no enforced version gate today** — no
  shipped client reads `hello.data.abi` at all; a client MAY compare it against
  an expectation and refuse, but a genuinely breaking bump would need clients to
  opt into that check first.

---

## Text message types

Five top-level `type` values. All JSON messages look like:

```json
{ "type": "<one of cmd|rsp|instances|log|event>", ...fields... }
```

> **Removed:** `vars` was a sixth `type`. The backend no longer collects or
> transports per-run script values, so this message is gone. Its old shape is
> kept below, struck through, only
> so existing consumers know what disappeared. The passive `VarKindWire` protocol
> enum still exists; it does not imply a live `vars` frame.

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

### ~~`vars` — backend to client~~ (REMOVED)

> **REMOVED.** The backend no longer tracks
> `VAR()` values or emits a `vars` message — `VAR()`/`EMIT()` still compile but
> publish nothing. Everything from here to the start of `instances` is retained
> only to document what the message used to look like; a current client receives
> none of it. Surfacing values/images for viewing now goes through the shipped
> **expose** plugin — build a `xi::Record`, tag it with `"$channel"`, and call
> `xi::use("expose").process(rec)`. See *The `expose` plugin* below.

Snapshot of a `ValueStore` after one `inspect()` call.

```json
{
  "type": "vars",
  "run_id": 17,
  "items": [
    { "name": "gray",    "kind": "image",   "gid": 100, "src": 100, "raw": false },
    { "name": "gray2",   "kind": "image",   "gid": 101, "src": 100, "raw": false },
    { "name": "blurred", "kind": "image",   "gid": 102, "src": 102, "raw": false },
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
- `kind` (string, required) — one of `image`, `number`, `boolean`, `string`, `json`, `custom`, `record`
- `value` (any) — inline value, present for non-image kinds. A non-finite
  `number` (NaN / ±Inf) is emitted as the JSON **string** `"NaN"` / `"Infinity"`
  / `"-Infinity"` (JSON has no non-finite numbers; emitting a bare `nan` would be
  invalid JSON and drop the whole frame). The same sentinel convention applies to
  non-finite doubles in a `Record` — they round-trip back to the non-finite value
  via `get_double`/`as_double` instead of silently reading as `0.0`. **Consumers
  must restore the sentinel back to a real number**: a top-level `kind: "number"`
  string value, *and* — because a `Record` can hold a non-finite field at any
  depth — recursively inside a `kind: "record"` `data` object/array. The
  reference implementation is `_restore_nonfinite` in `examples/lib/xex1.py`
  (recursive; applied when decoding `expose` frames — see below). Any consumer
  that reads a non-finite field must apply the same restore itself; one that
  skips it reads the literal string `"NaN"`, so a threshold compare silently
  misfires (JS) or raises `TypeError` (Python). **Ambiguity inside record `data`:** the record's
  JSON carries no per-field type tag, so a *genuine string field* whose value is
  exactly `"NaN"` / `"Infinity"` / `"-Infinity"` is indistinguishable from a
  non-finite double and will be restored to the number. This is an accepted
  trade-off (such literal strings are vanishingly rare in measurement data); if a
  record field must hold that literal text, wrap or prefix it so it isn't an exact
  sentinel match.
- `gid` (int) — present for `image` kind; unique per image var. Matches a binary
  preview frame's `gid` **only for the canonical var of its group** (see `src`).
- `src` (int) — present for `image` kind; the **canonical gid** of this image's
  group. Vars over the same underlying buffer (the "one frame VAR'd by every
  plugin/stage" case — in-process pass-by-pointer means they share `Image::data()`)
  all report a common `src`. The backend encodes + sends **exactly one** preview
  frame per `src` group (under the canonical var's `gid`); a client maps the
  frame's `gid` back to its `src` and mirrors the one decoded image onto every var
  in the group, so the image is JPEG-encoded once and decoded once instead of N
  times. For a non-duplicated image `src == gid`. (Record sub-images are not
  deduplicated and carry no `src`.)
- `raw` (bool) — `true` if the image is transmitted uncompressed (BMP), `false` for JPEG (currently always `false` — see *Binary frame layout*)
- For `kind: "record"` (a `xi::Record` VAR): `data` (object) holds the record's
  scalar fields; `image_keys` (array) lists its sub-image keys; `images` (object)
  maps each sub-image key → `gid`. Sub-images are gated by the **record var's**
  name (subscribe the record name to stream them) and are **not** deduplicated,
  so they carry no `src`. A consumer that only handles `kind: "image"` will skip
  record sub-images — map over `images` to render them.

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
{ "type": "event", "name": "run_finished", "data": { "run_id": 17, "ms": 42, "inspect_compute_us": 42137 } }
{ "type": "event", "name": "run_error", "data": { "run_id": 17, "what": "..." } }
{ "type": "event", "name": "run_result", "data": { "code": -2, "msg": "edge chip", "run_id": 17, "ms": 42, "source": "cam0", "group": "high" } }
{ "type": "event", "name": "script_reloaded", "data": { "path": "..." } }
{ "type": "event", "name": "state_dropped", "data": { "old_schema": 1, "new_schema": 2 } }
{ "type": "event", "name": "compile_started", "data": { "path": "..." } }
{ "type": "event", "name": "compile_finished", "data": { "path": "...", "ok": true } }
```

`run_started` / `run_finished` bracket every `cmd:run` and every
inspect dispatched by continuous mode. `run_started.data` carries
`{run_id}`; `run_finished.data` carries `{run_id, ms, inspect_compute_us}`.

> **BREAKING (staged, not on master).** The timing on `run_finished` is script
> **inspect COMPUTE time only** — it EXCLUDES queue wait, emit-gate wait, staged
> sink flush, JPEG encode and WS send. It is NOT cycle/decision latency. The
> legacy `ms` (integer ms) field is retained with its exact old value, but the
> additive `inspect_compute_us` (microseconds) field states that meaning
> explicitly. Consumers should migrate to `inspect_compute_us` and must NOT read
> `ms` as production/cycle rate (external review 05 #7).

`run_error.data` is `{run_id, what}` and fires
INSTEAD of `run_finished` when the inspect throws (C++ exception or
SEH). Drivers waiting for run completion should listen for
`run_finished` OR `run_error` — exactly one fires per run.

`run_result` carries the run's **one verdict** (script-set via `xi::result`, or
`0` = NA if unset): `{code, msg, run_id, ms[, source, group]}`. Code convention:
`>0` ok-class, `0` NA, `-1…` ng-class, `<= -990000` framework system-fail enum.
It fires once per run (before `run_finished`) **and** once per
**dropped** trigger (queue overflow → `code: -999001` `XI_SYS_DROPPED`, with no
`run_id`/`ms`) — so a consumer sees one Result per trigger with no gaps. See
[`roadmap/run-result.md`](../roadmap/run-result.md).

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

## ~~Binary frame layout — image preview~~ (REMOVED)

> **REMOVED.** The backend no longer encodes
> or sends the *core* image-preview binary frame. The layout below is retained
> only as a record of the old wire format; no current code path produces it.
> Image surfacing is handled by the shipped **expose** plugin, which pushes its
> own atomic `XEX1` binary frame via the ABI v8 `emit_binary` host call (see *The
> `expose` plugin* below and [`c-abi.md`](c-abi.md)).

One WebSocket binary frame per **distinct image** (per `src` group, not per image
var — see `vars.items[*].src`), sent after the `vars` message that introduces it.
Several image vars sharing one buffer produce a single frame; the client mirrors
it onto the whole group via `src`.

```
offset  size  field
  0     4B    gid        (uint32, big-endian)  — the canonical var's gid (vars.items[*].gid where src==gid)
  4     4B    codec      (uint32, big-endian)  — 0=JPEG, 1=BMP, 2=PNG
  8     4B    width      (uint32, big-endian)
 12     4B    height     (uint32, big-endian)
 16     4B    channels   (uint32, big-endian)  — 1 | 3 | 4
 20     N     payload    — codec-dependent bytes
```

Total header = 20 bytes. Clients read the 20-byte header, then consume the
remainder of the frame as the payload.

> **Current implementation emits JPEG only.** Every preview path encodes with
> `encode_jpeg` (quality 85 for run/var previews) and
> sets `codec = 0` (JPEG); `vars.items[*].raw` is therefore always `false`. The
> `BMP` (1) / `PNG` (2) codec values and the `raw` flag are reserved in the wire
> format but not produced today. A client may branch on `codec` defensively, but
> in practice can treat the payload as JPEG.

Rationale for including width/height/channels in the header: JPEG decoders
on the UI side need dimensions up front for layout, and embedding them lets
the client allocate image buffers before decoding. BMP already has this
metadata in-band; for JPEG we want it out-of-band for speed.

---

## The `expose` plugin (script data-out)

`expose` is the shipped replacement for the removed `vars` message + core image
preview — the official surface for getting arbitrary script values + images out
for a UI or external program. A script builds a plain `xi::Record`, tags it with a
string **channel id** under the reserved key `"$channel"`, and calls
`xi::use("expose").process(rec)`. Output is organised by channel (created
implicitly on first send); a channel is the unit of subscription and of the UI
tab. The host also stamps `"$seq"` (= the run id) for ordering; the plugin strips
both reserved keys from the published payload. See
[`../guides/write-a-script.md`](../guides/write-a-script.md) for the script side.

The transport has two halves: **subscription** rides the plugin's `exchange`
(there is **no** backend WS subscribe command — the core's `emit_binary` is a dumb
broadcast pipe with no server-side routing), and **delivery** is one atomic
`XEX1` binary frame per record.

### Subscription — over `exchange_instance`, not a backend command

A consumer subscribes by channel id; the plugin only JPEG-encodes + pushes a
channel's record when that channel has a subscriber (no subscriber → no encode, no
push). Frames still broadcast on the wire; each client filters by the frame's
`channel`. All four verbs go through `exchange_instance` (see below) against the
`expose` instance:

```json
{ "type":"cmd", "name":"exchange_instance",
  "args": { "name":"expose", "cmd": { "command":"subscribe",   "channels":["lane","high"] } } }
{ "type":"cmd", "name":"exchange_instance",
  "args": { "name":"expose", "cmd": { "command":"unsubscribe", "channels":["lane"] } } }
{ "type":"cmd", "name":"exchange_instance",
  "args": { "name":"expose", "cmd": { "command":"get", "channel":"lane" } } }
{ "type":"cmd", "name":"exchange_instance",
  "args": { "name":"expose", "cmd": { "command":"list_channels" } } }
```

- `subscribe` / `unsubscribe` — add/remove channel ids from the push set.
- `get` (pull latest) — returns `{ found, channel, seq, frame_b64 }`, where
  `frame_b64` is base64 of the **same `XEX1` frame**; decode it with the same
  decoder. `expose` keeps exactly one latest frame per channel (plugin state, not
  core). A late joiner is blank until the next run (snapshot-on-subscribe is not
  done) — it can `get` the latest explicitly.
- `list_channels` — returns channel/tab metadata.

### The `XEX1` binary frame (one record = one atomic frame)

Per run, for **each subscribed channel**, the plugin pushes one self-contained
binary frame via `emit_binary` (broadcast, ordered by `$seq`). No cross-message
reassembly — a consumer always receives a whole record atomically.

```
[ magic "XEX1" ][ msgpack body ]

body = {
  v:       1,                     // version — decoder gate
  channel: "lane",
  seq:     <run_id>,              // ordered-sink $seq, ordering/correlation
  json:    "<record scalars serialized to a JSON string>",  // original key order
  images:  [ { key: "edges", jpeg: <bin> }, ... ]           // each image JPEG-compressed
}
```

- **values:** the record's scalar tree is dumped to a single JSON string (the
  decoder does one `JSON.parse` / `json.loads`); key order is the record's
  insertion order = display order.
- **images:** all JPEG, carried as msgpack `bin`. JPEG is lossy / 8-bit
  gray|BGR — `expose` is the lightweight, preview-oriented surface;
  non-8-bit / lossless image transport is out of scope (use a purpose-built
  plugin). Image keys in the record correspond to entries in `images[]`.
- **version gate:** a decoder checks the `XEX1` magic + `v` and rejects anything
  else (closes the old XPV1-vs-header decoder drift). The stock decoder is
  `decode_xex1` in `examples/lib/xex1.py` (checks magic + `v`, raises otherwise;
  also does the non-finite restore above). The Python client
  (`tools/xinsp2_py/xinsp2/client.py`) stays **content-agnostic** — it queues raw
  binary frames on `_inbox_binary` for the caller to decode (e.g. via
  `xex1.decode_xex1`), rather than decoding `XEX1` itself.

---

## Commands

The backend implements ~60 commands. The core commands are documented in detail
below; additional commands are listed at the end of this section. Arguments are
listed under each entry.

### `ping`
`args: {}` → `data: { "pong": true, "ts": <unix_seconds> }`

### `version`
`args: {}` → `data: { "version": "0.1.0", "abi": 1, "commit": "abc123" }`

`abi` here is the **WS protocol** version — distinct from the C plugin-ABI struct
version `XI_ABI_VERSION` (**11**, see `reference/c-abi.md`). It is a hardcoded
constant `1` (emitted verbatim by the backend in `service_main.cpp` /
`service_cmd_lifecycle.cpp`); it has **never been bumped** and **no code reads or
enforces it** — treat it as an informational stamp, not a live version gate.

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
(and the client's subscriptions) intact rather than wedging to a null
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
`args: { "frame_path": "..." (optional), "meta": { ... } (optional) }`
→ `data: { "run_id": <int>, "ms": <int> }`
followed by a `run_result` event (and the `run_started`/`run_finished` brackets).

> The old `vars` message + binary previews that used to follow a run are
> **removed**. A `cmd:run` no longer streams
> script values back; observe the run via the `run_*` events. Per-run value /
> image surfacing is handled by the shipped `expose` plugin (one atomic `XEX1`
> frame per subscribed channel — see *The `expose` plugin* below).

When `frame_path` and/or `meta` are given, `cmd:run` builds a one-shot **record**
host-side (`frame_path` → an image under the key `"frame"`; `meta` → the metadata
doc) and exposes it as this run's `xi::current_trigger()` — the script reads it
via `current_trigger().image("frame")` / `.meta()`, exactly as if a source had
`emit_record`'d it, but with no source plugin and no continuous mode (headless
single-shot). A plain `cmd:run` (neither arg) leaves `current_trigger()` inactive.

`cmd:run` is the **deterministic single-shot** path (UI "Run", step-through). It
is rejected while continuous mode is active (`"cannot run while continuous mode
is active"`) and rapid runs are serialized so their `run_result`/`run_*` events
arrive in `run_id` order. Burst/throughput parallelism is the continuous-mode dispatch
pool's job (`parallelism.dispatch_threads` + the trigger bus / fps) — `cmd:run`
does not fan out.

`frame_path` is plumbed to the script as `xi::current_frame_path()`
(see `docs/guides/write-a-script.md`). Empty / missing means the
script gets an empty string. Combine with `xi::imread()` to load a
file frame on demand without a custom source plugin.

### `start` / `stop`
`start args: { "fps": int (default 10) }` → `data: { "started": true,
"dispatch_threads": int }` (the int reflects the project's
`parallelism.dispatch_threads`, default 1; included so callers can
verify the pool size that just came up).
On already-running: `data: { "already": true }`.
**Continuous mode has two drivers — don't conflate them.** The real driver is
**triggers**: image sources call `emit_record()` and the lanes run `inspect()` per
record (a run with no source/trigger is meaningless). `fps > 0` additionally runs a
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
`found:false` (the HMI then keeps its static fallback). The read is size-capped
(8 MiB): a file over the cap replies `found:false, truncated:true` and omits the
`dashboard` body (a partial slurp would be invalid JSON), so a pathological or
corrupt dashboard file can never drive an unbounded allocation.

Continuous mode runs `parallelism.dispatch_threads` worker threads inside
the backend (default 1; see `docs/guides/write-a-script.md` → Parallel
dispatch for the pool, per-instance reentrancy, watchdog, and `result_order`).
Each tick comes from one of two sources:

- Each `emit_record()` from a source dispatches one inspect call.
- A wall-clock timer at the requested fps fires a fallback dispatch
  even when no record is queued. Scripts that read trigger images
  must guard `xi::current_trigger().is_active()` because timer-only
  ticks have no record attached.

A `run_result` event is emitted on each dispatch (same as for `cmd:run`); the
old per-dispatch `vars` message is **removed**. There is no per-frame rsp; the only ack for `start`
is the initial one.

`cmd:start` **resets** the per-run dispatch counters used by
[`dispatch_stats`](#dispatch_stats) — `dropped` and
`queue_depth_high_watermark` go to zero. Drivers
that snapshot `dispatch_stats` before *and* after a `cmd:start` and
subtract will get nonsense across run boundaries. Either record only
the AFTER snapshot, or treat the field values as scoped to the most
recent cmd:start window. (The `*_lifetime` fields are NOT reset — use
those for cumulative-over-uptime totals.)

### `dispatch_stats`
`args: {}` → `data: { ... }`. Snapshot of the dispatch queue's
health since the most recent `cmd:start`:

| Field | Meaning |
|---|---|
| `queue_depth_now` | current queue size at snapshot time |
| `queue_depth_cap` | configured `project.parallelism.queue_depth` |
| `queue_depth_high_watermark` | peak queue depth observed since last `cmd:start` |
| `overflow` | configured policy: `drop_oldest` (default) / `drop_newest` |
| `dispatch_threads` | configured `project.parallelism.dispatch_threads` |
| `dropped` | events dropped on overflow since last `cmd:start` (aggregate across lanes) |
| `dropped_lifetime` | events dropped over the whole **backend process uptime** — does NOT reset on `cmd:start` |
| `queue_depth_high_watermark_lifetime` | peak single-lane queue depth over the whole **process uptime** — does NOT reset on `cmd:start` |
| `malformed_cmd_rejected_lifetime` | malformed / unparseable command envelopes rejected by the dispatch shell over the whole **process uptime** — does NOT reset on `cmd:start` (see *Error handling*) |
| `last_emit_age_ms` | ms since ANY source last emitted a record (monotonic); `-1` if none yet. The "is the line still getting frames" signal — a stalled camera otherwise stops the line silently |
| `sources` | `[{ source, last_emit_age_ms }]` — per-source emit age, to spot WHICH of N cameras stalled. Scoped to the current project: the per-source list is pruned on a project/script-reload boundary (so source names from a closed project don't linger), then repopulates as the new project's sources emit |

A monitor/FE applies a source-rate-appropriate staleness threshold to
`last_emit_age_ms` (auto-alerting on a fixed threshold is the consumer's call —
the expected frame rate is source-specific). The per-run counters (`dropped`,
`queue_depth_high_watermark`) are reset on every `cmd:start` — sample after stop
for the end-of-run total, do not subtract a pre-start snapshot. The `*_lifetime`
fields are cumulative for the whole backend process, so an unattended monitor can
answer "how much have we dropped total" across run/restart boundaries (a restart
no longer reads as a clean line).

### `metrics`
`args: {}` → `data: { ... }`. Minimal observability snapshot: monotonic
per-frame counters plus a fixed-bucket inspect-compute histogram. Recorded once
per completed inspection in `run_one_inspection` (covering ok, throw, and crash
paths). **All values are cumulative over the whole backend process uptime** —
unlike `dispatch_stats`' per-run counters, they are **NOT** reset on `cmd:start`.
A monitor derives throughput / windowed compute stats by diffing two snapshots
itself (same contract as the `dispatch_stats` `*_lifetime` fields).

> **BREAKING (staged, not on master).** The histogram key was `latency_ms`; it
> is renamed to `inspect_compute_ms` (and `latency_ms.*` → `inspect_compute_ms.*`)
> because the recorded value is script inspect **COMPUTE** time only — it excludes
> queue wait, emit-gate wait, staged sink flush, JPEG encode and WS send, so it is
> NOT cycle/decision latency (external review 05 #7). The numeric values, bucket
> edges and shape are unchanged; only the key name changed.

```json
{ "frames_total": 1024, "frames_ok": 1020, "frames_error": 4,
  "inspect_compute_ms": {
    "count": 1024, "sum_ms": 8123.500, "mean_ms": 7.933,
    "buckets": [ {"le": 0.5, "count": 12}, {"le": 1.0, "count": 40},
                 "...", {"le": 5000.0, "count": 3}, {"le": "inf", "count": 1} ]
  } }
```

| Field | Meaning |
|---|---|
| `frames_total` | inspections recorded since process start (`= frames_ok + frames_error`) |
| `frames_ok` / `frames_error` | success / failure partition of `frames_total` |
| `inspect_compute_ms.count` | frames in the histogram (equals `frames_total`) |
| `inspect_compute_ms.sum_ms` | Σ per-frame inspect **compute** time, ms (kept in integer µs internally; here in ms) |
| `inspect_compute_ms.mean_ms` | `sum_ms / count`, or `0` when `count == 0` |
| `inspect_compute_ms.buckets` | non-cumulative counts; each entry counts frames with `compute ≤ le`, partitioned by the 13 ms edges (`0.5 … 5000`). The final `{"le":"inf"}` is the overflow bucket. All bucket counts sum to `frames_total`. |

### `list_instances`
`args: {}` → triggers an `instances` message.

### `list_plugins`
`args: {}` → `data:` JSON array, one entry per registered plugin:

```json
[
  { "name": "blob_analysis", "description": "...", "folder": "...",
    "has_ui": true, "loaded": true, "origin": "global",
    "manifest": { "params": [...], "inputs": [...], "outputs": [...] } }
]
```

`manifest` is present only if the plugin's `plugin.json` defines a
top-level `manifest` block (free-form; see
`docs/reference/c-abi.md`). Backend passes it through verbatim —
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

A plugin's (or script's) own `host_api->log(level, msg)` also feeds these
channels: **WARN** and **ERROR** lines are forwarded to a live `log` event
(so they reach a connected operator instead of dying on the backend's
unwatched stderr), and **ERROR** lines additionally land in this
`recent_errors` ring (`source: "plugin"`). DEBUG/INFO stay on stderr only.
This matters on an unattended PC where nobody is watching the console — a
plugin's self-diagnostics now surface over the WS.

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
Toolchain** (see [`guides/build-and-run.md`](../guides/build-and-run.md) §5).

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
in the pool right now. Between runs (once a run's images are released),
they typically drop to zero, which can be confusing. `cumulative` solves that:

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

### `get_instance_def`
Symmetric read of `set_instance_def` — returns an instance's full def JSON.
`args: { "name": "cam0" }` → `data: { ... }` (the def; `ok: false` if no such
instance). The def includes any assets the plugin round-trips through
`get_def`/`set_def` — e.g. a matcher's per-template `image_png_b64` — so
`get_instance_def` → `set_instance_def` is an exact round-trip, and looping it
over `list_instances` snapshots a whole project (the basis for portable config
bundles). Resolves backend (plugin-manager) instances first, then script-loaded
instances.

### `exchange_instance`
Generic passthrough to an instance's `exchange()` method — used by plugin
UIs that ship their own command vocabulary.
`args: { "name": "cam0", "cmd": { ... } }` → `data: <whatever the plugin returns>`

### `get_state`
Orchestrator read: the host-tracked instance state machine.
`args: { "name": "cam0" }` → `data: { "state": "created"|"active"|"faulted",
"last_error": "...", "crash_count": N }` (`ok: false` if no such instance).
`crash_count` counts `process()` faults (SEH crash / thrown exception) for this
instance — a plugin crash leaves the instance `active` and returns NA, so this is
how a host detects a per-instance crash LOOP (and alerts) rather than just seeing
"NA result" every frame. Coarse by design — the
host records only these three states, driven by the host-visible verbs
(`create_instance` → `created`; a successful `set_instance_def`/`commit_group` →
`active`; a failed one → `faulted` with `last_error`). Fine staging/ready
sub-state stays plugin-side via `exchange {command:"get_status"}`. An instance
that exists but has had no host-visible transition yet reads `created`.

### `prepare_instance`
Orchestrator **stage**: load a new config's heavy assets into an instance's
background staging slot, off the critical path — the live config keeps running.
`args: { "name": "cam0", "def": { ... }, "folder"?: "..." }` → `ok: true`. For a
plugin that opted into the ABI v7 `XI_PLUGIN_STAGED` path this calls its **ungated**
`prepare()` (runs concurrent with `process()`); otherwise it falls back to a gated
`set_def` (immediate swap — the tier-1 path). Pair with `commit_group` to swap the
staged config in frame-perfectly. Resolves backend instances first, then script-
loaded ones (which keep the `exchange {command:"prepare"}` convention).

### `commit_group`
Orchestrator **drain-barrier**: call the first-class `commit()` on a GROUP of
instances atomically w.r.t. inspection runs. The host quiesces dispatch + drains
in-flight runs so no `process()` is mid-flight, commits every target in that one
no-process window (no run ever sees a half-committed group), then resumes dispatch
at the prior fps — a config switch must not stop the stream.
`args: { "instances"?: ["a","b"], "group"?: "line1", "plugin"?: "binarize",
"cmd"?: { ... } }` → `data: { "results": [ { "name", "ok", "result" }, ... ] }`.
Overall `ok: false` if any target failed. Resolves backend instances first, then
script-loaded ones.

> **BREAKING (staged, not on master).** The reply gains a top-level `status`
> field: `"committed"` (all targets committed) or `"partial"` (any target
> faulted). On a **partial commit** the drain-barrier is now *dismissed* rather
> than resumed: continuous production stays **HALTED** on the half-applied group
> (old behaviour auto-resumed the stream on a mix of new+old config), and a sticky
> `@commit` config-fault status is latched for operator intervention. `ok: false`
> on partial is unchanged; the all-or-none commit **semantics** (sequential, no
> rollback) are unchanged. Delta: `data` gains `"status"`; partial no longer
> auto-resumes dispatch. Requires an app-team cutover to unhalt after intervention.

**Addressing.** Targets are the deduped union of an explicit `instances[]` (which
also covers script-side instances) plus selectors that expand against existing
backend-instance properties — no new schema: `group` (the instance's dispatch
group) and `plugin` (its plugin type). Reusing `group`+`plugin` is the zero-schema
choice covering the common cohorts ("all of line1", "all binarize"); a dedicated
per-instance tag would only be needed if a config-switch cohort must cut ACROSS
dispatch groups.

This pairs with the **double-slot `prepare`/`commit`** a heavy-resource plugin
implements (ABI v7 `XI_PLUGIN_STAGED`): `prepare_instance` loads the new assets
into a background staging slot (the live config keeps running); `commit_group`
then swaps them in across the group. The expensive load happens off the barrier,
so the barrier is one in-flight run (~ms), not a stall. See
[`roadmap/config-bundles-and-orchestration.md`](../roadmap/config-bundles-and-orchestration.md)
for the full model, [`c-abi.md`](./c-abi.md) §1 for the ABI, and
`plugins/config_swap_probe/` for the reference plugin.

### `save_project` / `load_project` / `open_project`
`args: { "path": "project.json" }` → `ok: true`

`load_project` / `open_project` reattach instances and restore Param
values, but **do NOT recompile the inspection script** — call
`compile_and_load` separately afterwards. Cold opens of a project with
N project-local plugins compile each plugin under `cl.exe` and can
take 30–120 s; clients should pass a long timeout for this command.

**Partial-restore reporting.** `load_project` is a *succeeded-with-warnings*
operation: a clean restore replies `ok: true` with no data, but if any saved
Param value or instance `def` failed to apply the reply carries
`data: { "param_warnings": [...], "instance_warnings": [...] }` (arrays of
`"<name>: <reason>"` strings). A non-empty list means the recipe was only
partially restored — the client MUST surface it rather than treat the load as a
clean success. This exists to avoid a *fail-reads-as-pass*: instance defs
include script-declared `xi::Instance` objects (resolved via the script DLL's
own registry), so a dropped def would otherwise let a line run on default
thresholds/models while the operator believes the saved recipe applied.

> **BREAKING (staged, not on master).** `load_project` is now honest about
> partial application via a top-level `status` field: `"ok"` (every param +
> instance applied — `ok: true`), `"partial"` (some failed, warning arrays
> non-empty — now **`ok: false`**, was `ok: true`), or `"rejected"` (hard
> pre-parse failure: missing/unreadable `path`, invalid JSON — `ok: false` with
> `data: { "status": "rejected" }`). Delta vs old: a partial restore was
> previously reported as `ok: true` with warnings; generic clients (`if (resp.ok)
> show "loaded"`) read a half-applied recipe as success. Now partial/rejected set
> `ok: false`. The `param_warnings`/`instance_warnings` arrays are unchanged
> (additive). This is BEST-EFFORT / NON-ATOMIC import (no quiesce, no rollback);
> true atomic-recipe application is deferred.

**Corrupt `project.json` is quarantined, not silently rebuilt.** If
`project.json` exists but is **non-empty and unparseable** (e.g. a trailing comma
or a truncated write), `open_project` does NOT refuse the project — it opens in a
**degraded / read-only** mode (analogous to a compile failure: the process stays
up so headless autostart never hard-crashes on a bad file) with all parallelism /
runtime / groups defaulted, and surfaces an `open_project_warnings` entry whose
reason begins `"project.json is not valid JSON - opened READ-ONLY/degraded; saves
are blocked …"`. Two guarantees protect the on-disk bytes:

- **Bytes preserved.** The original file is copied verbatim to a sibling
  `project.json.corrupt-<ts>` (timestamp from the portable monotonic clock helper)
  at open time, so the operator's recoverable content is never lost.
- **Destructive saves blocked.** While the degraded flag is set, any instance CRUD
  (`create_instance` / `remove_instance` / `rename_instance`) is allowed to update
  the *instance* folders, but the full `project.json` rebuild is **refused** — it
  would otherwise overwrite the file with a defaults-only document and drop the
  top-level keys this backend does not emit but another writer owns (the VS Code
  extension's `params`, `auto_respawn`, `watchdog_ms`). The flag is cleared only by
  a *fresh successful `open_project`* of a now-valid file; a save never clears it.

  In `working_copy` mode the same guarantee holds end-to-end: the scratch
  `project.json` is never overwritten, so `commit_working_copy` mirrors the
  original bytes back onto the canonical (and carries the `.corrupt-<ts>` copy
  with it) — the corruption is contained, never propagated as data loss.

  To recover: fix (or restore from `project.json.corrupt-<ts>`) the JSON on disk
  and `open_project` again. See [`examples/qa_corrupt_project_json`](../../examples/qa_corrupt_project_json).

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
[`guides/write-a-script.md`](../guides/write-a-script.md) → "Using an external
library / DLL" and [`examples/script_external_dll`](../../examples/script_external_dll).

**Working-copy mode.** `open_project` accepts `"working_copy": true`: the
backend then operates on a `<project>/.xinsp_work` scratch copy (resume if
present, else seed from the canonical project), so edits are transactional and
crash-durable. If seeding the scratch fails (disk full, a locked/denied file),
the open is **aborted** (`open_project` returns an error) and the partial scratch
is removed — a torn seed must never become an authoritative, committable working
copy, or the eventual commit's mirror would prune the canonical files that merely
failed to copy in (silent data loss). Removing the partial scratch also keeps a
later crash-resume from adopting it. The reply data then carries `"working_copy": true`,
`"canonical_path"`, and `"working_dir"`. Two paired commands:
- `commit_working_copy` `args: {}` → mirror the scratch back onto the canonical
  project (add + overwrite + delete-removed). Reply `{ "committed": true, "canonical": "<dir>" }`.
  The commit is journaled with a `.xinsp_commit_pending` marker written (durably)
  before the mirror and cleared after it, so an interruption (crash / power loss
  mid-mirror) leaves a torn canonical that is detectable and recoverable.
- `discard_working_copy` `args: {}` → delete the scratch, re-seed from canonical,
  reopen. Reply is the project JSON (same shape as `open_project`).

**Crash recovery + Discard.** A surviving `.xinsp_commit_pending` marker means a
prior commit was interrupted: the canonical tree may be torn, and the intact
scratch (never modified by the mirror) is the **only** source that can heal it.
`open_project` rolls such a commit *forward* on open — it re-runs the idempotent
mirror (scratch → canonical) to complete the commit, then clears the marker.
**`discard_working_copy` honours this first.** Discard's contract is "throw away
my *uncommitted* edits", but a pending commit is **not** uncommitted edits — it
is a half-applied commit the user already requested. So Discard **completes the
interrupted commit from the scratch before dropping it**, never leaving the
canonical torn (rolling the commit *back* is impossible — the pre-commit
canonical bytes are already partially overwritten). Only once the canonical is
healed (or there was no pending commit) does Discard remove the scratch and
re-seed a fresh working copy. If the heal mirror itself fails (persistent disk
error), the scratch + marker are kept so a later open retries — the only recovery
source is never discarded.

See [`guides/deploy.md`](../guides/deploy.md). The
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

### ~~`subscribe` / `unsubscribe`~~ (REMOVED)

> **REMOVED.** These commands gated which
> VAR-image previews were encoded and streamed. With the `vars` message and the
> binary preview frame gone from core, there is nothing to subscribe to —
> sending `subscribe`/`unsubscribe` now replies `ok:false` (unknown command).
> Per-run output surfacing (and the encode-only-when-watched optimisation) is
> handled by the shipped **expose** plugin, which owns its own subscription
> model — its `exchange_instance` commands + `emit_binary` `XEX1` push, **not** a
> backend WS command. See *The `expose` plugin* below.

> The backend keeps **no run-history ring**, and no longer pushes any per-run
> value frame. Recent-run scrollback (and SPC-style backfill) is a client-side /
> plugin concern.

---

## Error handling

- Malformed / unparseable envelope: if a numeric `id` is still recoverable from
  the payload (e.g. a valid client that sent one bad field, or the wrong `type`),
  the backend replies `rsp` with `ok: false, error: "malformed command"` carrying
  that `id`, so the client is not left blocking to its request timeout. When no
  `id` can be recovered (none present, or one that overflows int64) there is
  nothing to correlate to, so the backend falls back to a `{ "type": "log",
  "level": "error", "msg": "malformed cmd: ..." }` line. Either way the connection
  stays open and a process-uptime reject counter
  (`dispatch_stats.malformed_cmd_rejected_lifetime`) is incremented.
- Unknown command name: `rsp` with `ok: false, error: "unknown command: xyz"`.
- Exception inside a command handler: `rsp` with `ok: false, error: <what()>`.
  A single top-level guard around command dispatch converts **any** exception
  escaping a handler (including `std::bad_alloc`) into this structured reply
  correlated to the command `id`, rather than letting it unwind out of the serve
  loop and terminate the backend.
- Script runtime exception: emitted as an `event` with `name: "run_error"` and `data: { "what": "..." }`. The `rsp` for the `run` command still returns `ok: true` if the run started — failure is reported via the event channel so partial vars can still be delivered.

---

## Status channel

Components publish a short, sticky "what am I doing right now" string (distinct
from per-inspection script *values* and from the `log`/`recent_errors` *event
stream* — status is one last-value string per component, overwritten in place).

- A script calls `xi::status("waiting for trigger")` (include `<xi/xi_status.hpp>`).
  The host stores it under the key `@script`.
- A plugin calls `status("grabbing")` (the `xi::Plugin::status` helper, or the
  `host_api->set_status(source, text)` C ABI). The host keys it by the instance
  name (e.g. `cam0`). ABI-additive — older plugins/hosts simply don't use it.
- The host itself publishes `@compile` — the health of the last `compile_and_load`.
  `text == "ok"` after a successful load; a `"degraded: …"` string after a failed
  attempt (compile error, bad DLL, out-of-tree prebuilt). On a mid-run hot-reload
  the failure reply (`ok:false`) only reaches the *calling* client and the line keeps
  streaming the last-good DLL, so this marker is how an unattended operator (or a
  reconnecting one) detects that the running def is stale/degraded. A later successful
  recompile clears it back to `"ok"`. The entry's `seq`/`ts_ms` double as a
  running-def generation + recency stamp.
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
| `crash_reports` | Return the JSON crash reports written by previous fatal crashes (from `%TEMP%/xinsp2/crashdumps/*.json`), newest-first. Each report body is read with an 8 MiB size cap; a file over the cap is listed as `{ "file", "truncated": true }` with no `report` body instead of being slurped unbounded. |
| `clear_crash_reports` | Delete all crash JSON files from the dump directory. Reply: `{ "removed": N }`. |

#### Watchdog

| Command | Purpose |
|---|---|
| `set_watchdog_ms` `args: { "ms": N }` | Set the per-inspect wall-clock budget in ms (0 = disabled). Reply: `{ "ms": N, "trips": N }`. |
| `watchdog_status` | Current watchdog config and trip count. Reply: `{ "ms": N, "trips": N, "armed": bool }`. |

#### Runtime tuning (live, mirrors `project.json` `runtime.*`)

| Command | Purpose |
|---|---|
| `set_process_priority` `args: { "class": "high" }` | Set the live OS process priority class: `high` \| `above` \| `normal` \| `below` \| `realtime`. Mirrors the `--priority` flag / `project.json` `runtime.process_priority`. Reply: `{ "process_priority": "<class>" }`. |
| `set_timer_fps` `args: { "fps": N }` | Set the live synthetic-tick rate for continuous mode (`fps <= 0` = trigger-only, no ticks). Takes effect on the next timer loop while running; the UI persists it to `project.json` `runtime.timer_fps`. |

#### Pipeline graph capture

| Command | Purpose |
|---|---|
| `graph_capture` `args: { "enable": bool }` | Toggle dataflow edge recording (default off — no hot-path cost). Clears any prior recording on enable. Reply: `{ "capturing": bool }`. |
| `graph_snapshot` | Reconstruct dataflow edges from the recorded calls by image-handle identity (instance A produced handle H; instance B consumed H → A→B edge). Reply: `{ "capturing": bool, "ran": ["inst", ...], "edges": [{ "from": "A", "to": "B", "keys": ["mask"] }, ...] }`. The `ran` list is in call order; `keys` lists the output-image names that crossed the edge. |

#### Params (out-of-band operations)

| Command | Purpose |
|---|---|
| `list_params` | List all registered `xi::Param` values (name, type, value, min/max if numeric). |

#### Plugin management

| Command | Purpose |
|---|---|
| `rescan_plugins` | Rescan the global plugins directories and refresh manifests. Does not reload already-loaded plugin DLLs. |
| `load_plugin` `args: { "name": "...", "folder"?: "..." }` | Force-load (or reload) a specific plugin by name. Typically used after `rescan_plugins` found a new plugin. |
| `rebuild_plugins` `args: { "cmake"?: "...", "config"?: "Release", "plugins"?: ["a","b"] }` | For every `build: cmake` plugin whose source changed (or just the named `plugins`): unload it, run its own CMake build, then reload the DLL and restore instances. Runs in three phases — unload all changed, **build them in parallel**, reload each — so a multi-plugin round is fast and each is unloaded only briefly. Reply `data: { "plugins": [{ "plugin", "status": "rebuilt"\|"unchanged"\|"failed", "detail" }] }`. Unchanged plugins (sources older than their built DLL) are skipped. The unload→build→load order is required on Windows (a loaded DLL can't be overwritten; CMake emits a fixed-name DLL) — which is why CMake runs host-side. A plugin that didn't truly unload (lingering worker thread / GPU context) is reported `failed` rather than silently left on stale code. |
| `export_project_plugin` `args: { "name": "..." }` | Package a compiled project-local plugin (DLL + manifest) for distribution; stamps `abi_version` in the exported `plugin.json`. |
| `unquarantine_plugin` `args: { "name": "..." }` or `{ "dir": "..." }` | Operator un-quarantine (Part III G2.3). Clears the certify verdict (crashed/quarantined) in a plugin's `.xi_certify.json` so the next scan re-certifies it from scratch, then re-scans so a now-clean plugin is re-armed without a restart. Resolve by plugin `name` (via the last scan) or explicit folder `dir`. |

#### Project and instance CRUD

| Command | Purpose |
|---|---|
| `create_project` `args: { "path": "...", "name": "..." }` | Create a new empty project folder with a stub `project.json` and `inspect.cpp`. |
| `close_project` | Tear down all instances and the dispatch lanes. Does not unload script or plugin DLLs. |
| `create_instance` `args: { "plugin": "blob_analysis", "name": "det0" }` | Add a new instance to the open project (creates folder, calls `xi_plugin_create`, writes `instance.json`). |
| `remove_instance` `args: { "name": "det0", "delete_folder"?: bool }` | Remove an instance from the registry and call `xi_plugin_destroy`. `"delete_folder": true` deletes the on-disk folder. Default (`false`) keeps the folder but moves its `instance.json` aside to `instance.json.removed` — open_project discovers instances by folder scan, so the removal still has to persist or the instance would resurrect on reopen; the tombstone keeps the config/assets recoverable. |
| `rename_instance` `args: { "name": "det0", "new_name": "detector" }` | Rename an instance (moves its folder, updates the registry). On a disk-save failure the runtime is still renamed; the reply is an error noting it may revert on restart (not "rename failed"). |
| `save_instance_config` `args: { "name": "det0" }` | Write the current `get_def()` output for one instance to `instance.json` without a full `save_project`. |
| `get_project` | Return the open project's `project.json` content and resolved metadata. |
| `get_plugin_ui` `args: { "name": "blob_analysis" }` | Return the plugin's `ui/index.html` content (for plugins with `has_ui: true`). Used by the VS Code extension to open the plugin webview. |

#### Dispatch stats

`dispatch_stats` is already documented above under `start` / `stop`.

---

## Backend command-line flags

`xinsp-backend.exe` is normally launched by a supervisor (the VS Code
extension, or `xinsp-fe.exe` on a line — see
[`internals/fe-be.md`](../internals/fe-be.md)). Key flags:

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
| `--working-copy` | off | open via a `<project>/.xinsp_work` scratch (transactional; resumes on crash respawn) — see [working-copy guide](../guides/deploy.md) |
| `--priority=CLASS` | (unchanged) | process priority class (Win): `high`/`above`/`normal`/`below`/`realtime`. `realtime` can starve the OS — use with care |
| `--aot` | off | prebuilt bundle: load existing plugin/script DLLs, **never invoke the compiler** (a `.dll` `script` path loads directly; plugins load the newest `build/*.dll`). See [`guides/deploy.md`](../guides/deploy.md) export bundle |

Performance notes: on Windows the backend raises the OS timer resolution to **1 ms**
(`timeBeginPeriod(1)`) at startup so sleeps / timer-tick fps / CV waits are tight,
and **logs a warning** if the total dispatch worker count (Σ per-group
`max_parallel`, or `dispatch_threads`) exceeds the core count (oversubscription →
context-switch thrash). See [`internals/dispatch.md`](../internals/dispatch.md)
for per-group `thread_priority` / `cpu_affinity`.

The `--project` autostart drives the same `open_project → compile_and_load →
start` commands a client would send, by synthesizing them internally after the
WS port binds. No client need ever connect (reply frames are no-ops with no
client), and the port stays open so an operator HMI / the extension can attach
live. This lets `xinsp-fe.exe` run a line at the process level without a C++ WS
client.

## Remote mode & auth

By default the backend binds to **loopback only** (`127.0.0.1`). To expose it on a
LAN / factory network, set the bind address and an auth secret — via flags or env
(env is preferred on shared hosts; argv leaks to `ps`/Task Manager):

```bash
xinsp-backend.exe --host=0.0.0.0 --port=7823 --auth=<secret>
#  or:  XINSP2_HOST=0.0.0.0  XINSP2_AUTH=<secret>  xinsp-backend.exe
```

The secret gates the WebSocket **handshake** (not per-frame). Two modes:

- **Plain bearer** (`--auth=<secret>`) — the client sends `Authorization: Bearer
  <secret>`; the server does a constant-time compare. Anyone who sniffs the
  handshake can replay it forever, so this is only safe on a trusted network
  (loopback / VPN / SSH tunnel).
- **HMAC challenge** (`--auth=hmac:<key>`) — the rest of the secret is the HMAC
  key. The client sends two headers:

  ```
  X-Xi-Timestamp: <unix_seconds>
  Authorization: Bearer <hex(hmac_sha256(key, "<unix_seconds>"))>
  ```

  The server requires the timestamp within **±60 s** of now **and** the HMAC to
  match (constant-time). The replay window is 60 s instead of forever.

Either way the WS frames **after** the handshake are plaintext — there is no
built-in TLS. For a hostile network terminate TLS upstream (nginx / caddy)
regardless of mode. Binding `--host=0.0.0.0` **without** `--auth` prints a
prominent stderr warning: the secret is the only access gate.
