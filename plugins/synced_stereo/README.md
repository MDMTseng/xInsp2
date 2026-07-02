# synced_stereo

Synthetic stereo camera: a **gathering source** that grabs both cameras and
emits `left` + `right` in ONE record under a single trigger. Multi-camera sync
needs no bus policy — the frames are correlated because they ride the same
record. Left is vertical stripes, right is horizontal, both stamped with the
same `seq` in their top-left pixels so a script can verify they came from the
same event.

Being a **source**, it has no `process()` input Record — its script/UI-facing
contract is its **config** (`get_def`/`set_def`), its **commands** (`exchange`),
and the two-image **frame** it emits. It follows the plugin data contract
([docs/new_gen/02-plugin-data-contract.md](../../docs/new_gen/02-plugin-data-contract.md))
on those surfaces: a typed builder for config/commands and a typed extractor for
the emitted stereo frame.

## Keys — one source of truth

Every key name is declared **once** in
[`../../contract/plugins/synced_stereo.decl.json`](../../contract/plugins/synced_stereo.decl.json),
from which `contract/codegen/gen_contract.py` generates `synced_stereo_keys.gen.h`
(the `keys::` constants) and `synced_stereo_schema.gen.h` (the left+right frame
slots). The typed view and (once the `spawn_worker` port lands — see below) the
plugin's own readers compile from those generated constants. The `_io.h` here
stays **hand-written** (the decl sets `"handwritten_io": true`): its derived
`has_both()` and defaulted `fire(int n = 1)` are outside what the generator emits
verbatim — a documented codegen gap (see `contract/codegen/README.md`, "Coverage &
the codegen gap").

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `fps`     | int  | settable; clamped to [1, 120] |
| config  | `running` | bool | read-only (`get_def` only) |
| config  | `ticks`   | int  | read-only (`get_def` only) — frames emitted |
| command | `command` | string | `start` / `stop` / `fire` / `set_fps` |
| command | `value`   | int  | **required** for `set_fps` |
| command | `n`       | int  | frame count for `fire` (headless drive) |
| output  | `left`    | image | vertical stripes |
| output  | `right`   | image | horizontal stripes |

Schema version: `xi::synced_stereo::kSchemaVersion` (currently **1**). The
`Config` builder stamps it so a header/plugin skew reports precisely.

> **Adoption note.** `synced_stereo.cpp` is *not yet* compiled against these
> constants: its worker/emit path is being ported to the blessed `spawn_worker`
> on a parallel branch, and this task deliberately does not touch that code to
> avoid a conflicting diff. The decl (and its generated keys header) is the
> documented source of truth for the typed view now;
> `tests/test_synced_stereo.cpp` **pins the wire keys** by
> reading a really-emitted frame through the extractor, so any drift between the
> plugin's literals and this header turns the test red. The `.cpp` adopts the
> constants when the `spawn_worker` port merges.

## Using it from a driver / script

Include [`synced_stereo_io.h`](./synced_stereo_io.h):

```cpp
#include "synced_stereo_io.h"

host.set_def(cam, xi::synced_stereo::Config().fps(30));
host.exchange(cam, xi::synced_stereo::Command::start());
host.exchange(cam, xi::synced_stereo::Command::fire(4));   // headless: emit 4 pairs

// Read an emitted pair with the typed extractor — both images live in one
// record, which IS the correlation guarantee.
xi::synced_stereo::Frame f{ emitted_record };
if (f.has_both()) correlate(f.left(), f.right());
```

## Tests

`tests/test_synced_stereo.cpp` asserts: the `Config`/`Command` builder happy path
(fps set + clamped through `get_def`); and that `fire` emits one record carrying
BOTH images, read end-to-end through the typed `Frame` extractor. Run via
`ctest -C Release -R synced_stereo_test` from `plugins/build`.
