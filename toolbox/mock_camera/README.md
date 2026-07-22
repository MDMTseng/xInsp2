# mock_camera

Simulated camera source. Streams RGB test frames (a shifting gradient with a
frame counter drawn top-left) on its own capture thread; configurable
width/height/fps; start/stop via commands. Each frame is emitted as ONE sealed
`xi.pack@1` Pack (v12: the sole data plane): the `frame` image entry (a
zero-copy `adopt_image` of the pool slot it was painted into) plus the `seq`
counter and the `gain` the frame was painted with.

Being a **source**, it has no `process()` *data* input — its script/UI-facing
contract is its **config** (`get_def`/`set_def`), its **commands**
(`exchange`), the **frame pack** it emits, and its **control door** (below).
It implements the plugin data contract
([docs/new_gen/02-plugin-data-contract.md](../../docs/new_gen/02-plugin-data-contract.md))
on those surfaces: generated key constants, fail-loud required command
payloads, and a config schema-version stamp.

## Keys — one source of truth

Every key name is declared **once** in
[`../../contract/plugins/mock_camera.decl.json`](../../contract/plugins/mock_camera.decl.json),
from which `contract/codegen/gen_contract.py` generates `mock_camera_keys.gen.h`
(the `keys::` constants the plugin's readers compile from) plus the TS/Py
typings and the docs keys table.

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `width`  | int | |
| config  | `height` | int | |
| config  | `fps`    | int | clamped to [1, 60] |
| config  | `streaming` | bool | read-only (`get_def` only) |
| config  | `gain` | double | default 1.0, clamped to [0.05, 8.0] — brightness multiplier |
| command | `command` | string | `start` / `stop` / `get_status` / `set_fps` / `set_resolution` / `set_gain` |
| command | `value`   | int/double | **required** for `set_fps` (int) and `set_gain` (double) |
| output  | `frame`   | image | emitted RGB frame |
| output  | `seq`     | int | frame counter |
| output  | `gain`    | double | the gain THIS frame was painted with |
| door    | `ack`     | string | door reply: echoes the applied command name |

Schema version: `xi::mock_camera::kSchemaVersion` (currently **1**). A config
built against a different version is rejected by `set_def` with a precise error
naming both versions. A config with no `_schema` stamp (e.g. a legacy persisted
`instance.json`) is tolerated.

## Using it from a driver / script

```cpp
host.set_def(cam, R"({"width":1280,"height":720,"fps":15})");
host.exchange(cam, R"({"command":"start"})");
host.exchange(cam, R"({"command":"set_fps","value":30})");
// Each dispatched trigger carries ONE sealed Pack — read it via t.pack()
// (entries {frame, seq, gain}) or drive the control door below.
```

## Failure shape

A command whose required payload is missing/mis-typed fails loud rather than
silently no-op'ing. `set_fps` with no `value` returns:

```json
{ "error": "missing_input", "key": "value", "expected_type": "int" }
```

(`error` is one of `missing_input` / `wrong_type` — the shared `xi::contract`
reason codes.) Unknown commands return the framework's
`{"error":"unknown_command", ...}` shape.

## Control door (closed loop)

Since polaris2 ex-feedback the source speaks the pack plane **both
directions**: besides *emitting* frame packs, it *accepts* control packs
through its own `xi.pack@1` door (`XI_PLUGIN_PACK_DOOR`). The door speaks the
same command vocabulary as `exchange()`, as pack entries:

```
{ command:"set_gain", value:f64 }   →  ack pack { ack:"set_gain", gain:<clamped>, seq:<frame counter> }
{ command:"get_status" }            →  ack pack { ack:"get_status", gain, seq, streaming }
```

A script closes the loop with `xi::use("cam").process(ctrl)` (request-reply;
the sealed door output IS the ack) or `.push(ctrl)` (fire-and-forget). The
commanded gain applies to the **next** emitted frame — frame-latency control;
each frame echoes the gain it was painted with (`gain` entry) so the loop can
regulate against the actual plant state. Faults are the pack-shaped fail-loud
contract: a value-less `set_gain` acks `$fault=missing_input`, a mis-typed
value `$fault=wrong_type`, an unknown selector `$fault=unknown_command` —
never a silent no-op.

The live exemplar is
[qa/qa_pack_feedback](../../qa/qa_pack_feedback/README.md)
(mean-intensity analysis in the script steering `gain` into a target band).

## Tests

The pack emit + chained-door flow is proven against the real DLL in
`toolbox/tests/pack_pilot_test.cpp` (pack_pilot_test); the script-surface door chain
in `toolbox/tests/use_pack_door_test.cpp`; the live closed loop in
`qa/qa_pack_feedback`. Run via `ctest --test-dir toolbox/build -C Release`.
