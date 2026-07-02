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

Every key name is defined **once** in
[`mock_camera_keys.h`](./mock_camera_keys.h); the plugin's readers and the typed
view both compile from it.

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `width`  | int | |
| config  | `height` | int | |
| config  | `fps`    | int | clamped to [1, 60] |
| config  | `streaming` | bool | read-only (`get_def` only) |
| command | `command` | string | `start` / `stop` / `get_status` / `set_fps` / `set_resolution` |
| command | `value`   | int | **required** for `set_fps` |
| output  | `frame`   | image | emitted RGB frame |

Schema version: `xi::mock_camera::kSchemaVersion` (currently **1**). A config
built against a different version is rejected by `set_def` with a precise error
naming both versions. A config with no `_schema` stamp (e.g. a legacy persisted
`instance.json`) is tolerated.

## Using it from a driver / script

Include [`mock_camera_io.h`](./mock_camera_io.h):

```cpp
#include "mock_camera_io.h"

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

## Tests

`tests/test_mock_camera.cpp` asserts: `set_fps` with no `value` → structured
fault; a future config `_schema` → `set_def` rejects it and leaves config
unchanged; and the `Config`/`Command` builder happy path (including fps
clamping). Run via `ctest -C Release -R mock_camera_test` from `plugins/build`.
