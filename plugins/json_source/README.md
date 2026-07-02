# json_source

Emits a configurable JSON record. The user edits the JSON via the plugin's GUI;
the stored object is produced as the output Record on every `process()` call,
optionally mutated by runtime patches carried in the input. Useful for injecting
test fixtures, configuration, or manual data into a pipeline.

## Open output, typed control surface

The JSON this plugin emits is **user-defined**, so its output data plane is an
**open schema** by design (docs/new_gen/02-plugin-data-contract.md — intrinsically
generic plugins declare open schemas). There is deliberately **no typed Output
extractor**: the produced record is whatever the user authored.

What *is* a fixed, declarable vocabulary is the plugin's **control surface** — its
commands, its config wrapper, and the patch shape `process()` accepts. Those key
names (hand-parsed with raw yyjson before) now follow the plugin data contract:

## Keys — one source of truth

Every control key is declared **once** in
[`../../contract/plugins/json_source.decl.json`](../../contract/plugins/json_source.decl.json),
from which `contract/codegen/gen_contract.py` generates `json_source_keys.gen.h`
(the `keys::` constants). The plugin's own readers and the typed view
([`json_source_io.h`](./json_source_io.h)) compile from those generated constants.
The `_io.h` here stays **hand-written** (the decl sets `"handwritten_io": true`):
its config/`set_data`/`Patch` builders splice raw user JSON — an OPEN control
surface by design, outside the typed field family the generator emits — a
documented codegen gap (see `contract/codegen/README.md`, "Coverage & the codegen
gap").

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `data`    | json | the stored user document (wrapped: `{ "data": … }`) |
| command | `command` | string | `set_data` / `reset` / `get_status` |
| command | `value`   | json | **required** for `set_data` (the new document) |
| input (patch) | `key`     | string | patch target path, e.g. `.a.b[2]` |
| input (patch) | `value`   | json | patch value (open) |
| input (patch) | `patches` | array | batch form: `[ {key,value}, … ]` |

Schema version: `xi::json_source::kSchemaVersion` (currently **1**). A config
built against a different version is rejected by `set_def` (naming both versions
in the log); a config with no `_schema` stamp is tolerated.

## Using it from a driver / script

```cpp
#include "json_source_io.h"

host.set_def(src, xi::json_source::Config().data(R"({"roi":[0,0,64,64]})"));
host.exchange(src, xi::json_source::Command::set_data(R"({"n":3})"));

// Per-emit patch — mutate one path for THIS emit only (open value, JSON text).
auto in_single = xi::json_source::Patch::single(".n", "7");
auto in_batch  = xi::json_source::Patch::batch({ {".a", "1"}, {".b", "\"x\""} });
```

## Failure shape

`set_data` with no `value` fails loud rather than silently keeping the old JSON:

```json
{ "error": "missing_input", "key": "value", "expected_type": "json" }
```

(`error` uses the `xi::contract` reason codes shared with the Record path.)

## Tests

`tests/test_json_source.cpp` asserts: `set_data` with no `value` → structured
fault; a future config `_schema` → `set_def` rejects it and leaves the stored
JSON unchanged; and the `Config`/`Command`/`Patch` builder happy path (a patch
mutates only the emitted record, read back with a raw key since the output is
open). Run via `ctest -C Release -R json_source_test` from `plugins/build`.
