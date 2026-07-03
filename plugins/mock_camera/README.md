# mock_camera

Simulated camera source. Streams RGB test frames (a shifting gradient with a
frame counter drawn top-left) on its own capture thread; configurable
width/height/fps; start/stop via commands.

Being a **source**, it has no `process()` input Record — its script/UI-facing
contract is its **config** (`get_def`/`set_def`), its **commands**
(`exchange`), and the **frame** it emits. It implements the plugin data contract
([docs/new_gen/02-plugin-data-contract.md](../../docs/new_gen/02-plugin-data-contract.md))
on those surfaces: a typed builder for config/commands, a typed extractor for
the emitted frame, fail-loud required command payloads, and a config
schema-version stamp.

## Keys — one source of truth

Every key name is declared **once** in
[`../../contract/plugins/mock_camera.decl.json`](../../contract/plugins/mock_camera.decl.json),
from which `contract/codegen/gen_contract.py` generates `mock_camera_keys.gen.h`
and the typed `mock_camera_io.gen.h`; the plugin's readers and the typed view
both compile from those generated constants.

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `width`  | int | |
| config  | `height` | int | |
| config  | `fps`    | int | clamped to [1, 60] |
| config  | `streaming` | bool | read-only (`get_def` only) |
| config  | `pack_mode` | bool | default false — emit via the xi.pack@1 plane instead of Record |
| config  | `gain` | double | default 1.0, clamped to [0.05, 8.0] — **pack-mode** brightness multiplier |
| command | `command` | string | `start` / `stop` / `get_status` / `set_fps` / `set_resolution` / `set_gain` |
| command | `value`   | int/double | **required** for `set_fps` (int) and `set_gain` (double) |
| output  | `frame`   | image | emitted RGB frame |
| output  | `seq`     | int | frame counter (pack-mode entry) |
| output  | `gain`    | double | pack-mode entry: the gain THIS frame was painted with |
| door    | `ack`     | string | door reply: echoes the applied command name |

Schema version: `xi::mock_camera::kSchemaVersion` (currently **1**). A config
built against a different version is rejected by `set_def` with a precise error
naming both versions. A config with no `_schema` stamp (e.g. a legacy persisted
`instance.json`) is tolerated.

## Using it from a driver / script

Include the generated `mock_camera_io.gen.h`:

```cpp
#include "mock_camera_io.gen.h"

// Config via the typed builder (stamps the schema version).
host.set_def(cam, xi::mock_camera::Config().width(1280).height(720).fps(15));

// Commands via the typed builder.
host.exchange(cam, xi::mock_camera::Command::start());
host.exchange(cam, xi::mock_camera::Command::set_fps(30));

// Read an emitted frame with the typed extractor.
xi::mock_camera::Frame f{ emitted_record };
if (f.has_frame()) show(f.image());
```

## Failure shape

A command whose required payload is missing/mis-typed fails loud rather than
silently no-op'ing. `set_fps` with no `value` returns:

```json
{ "error": "missing_input", "key": "value", "expected_type": "int" }
```

(`error` is one of `missing_input` / `wrong_type`, matching the `xi::contract`
reason codes used on the Record path.) Unknown commands return the framework's
`{"error":"unknown_command", ...}` shape.

## Control door (pack-mode closed loop)

Since polaris2 ex-feedback the source is **bilingual both directions**: besides
*emitting* pack frames (`pack_mode`), it *accepts* control packs through its
own `xi.pack@1` door (`XI_PLUGIN_PACK_DOOR`). The door speaks the same command
vocabulary as `exchange()`, as pack entries:

```
{ command:"set_gain", value:f64 }   →  ack pack { ack:"set_gain", gain:<clamped>, seq:<frame counter> }
{ command:"get_status" }            →  ack pack { ack:"get_status", gain, seq, streaming }
```

A script closes the loop with `xi::use("cam").process(ctrl)` (request-reply;
the sealed door output IS the ack) or `.push(ctrl)` (fire-and-forget). The
commanded gain applies to the **next** emitted frame — frame-latency control;
each pack-mode frame echoes the gain it was painted with (`gain` entry) so the
loop can regulate against the actual plant state. Faults are the pack-shaped
fail-loud contract: a value-less `set_gain` acks `$fault=missing_input`, a
mis-typed value `$fault=wrong_type`, an unknown selector
`$fault=unknown_command` — never a silent no-op.

This addition is strictly **additive**: the Record emit path is untouched
(never scaled, byte-for-byte the pre-door behavior), and a host that never
probes `xi_plugin_get_interface` sees exactly the plugin it always saw. The
live exemplar is [examples/qa_pack_feedback](../../examples/qa_pack_feedback/README.md)
(mean-intensity analysis in the script steering `gain` into a target band).

## Tests

`tests/test_mock_camera.cpp` asserts: `set_fps` with no `value` → structured
fault; a future config `_schema` → `set_def` rejects it and leaves config
unchanged; the `Config`/`Command` builder happy path (including fps and gain
clamping); and the control door (set_gain ack round-trip + clamp + fail-loud
`$fault` on a command-less pack). The script-surface door chain is covered in
`plugins/use_pack_door_test.cpp` (5c) and the live loop in
`examples/qa_pack_feedback`. Run via `ctest -C Release -R mock_camera_test`
from `plugins/build`.
