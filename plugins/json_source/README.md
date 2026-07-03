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
| config  | `pack_mode` | bool | opt into pack-plane emit (default `false`); see "Bilingual" below |
| output  | `seq`     | int  | per-emit counter (pack-mode entry, mirrors mock_camera) |
| command | `command` | string | `set_data` / `reset` / `get_status` |
| command | `value`   | json | **required** for `set_data` (the new document) |
| input (patch) | `key`     | string | patch target path, e.g. `.a.b[2]` |
| input (patch) | `value`   | json | patch value (open) |
| input (patch) | `patches` | array | batch form: `[ {key,value}, … ]` |

Schema version: `xi::json_source::kSchemaVersion` (currently **1**). A config
built against a different version is rejected by `set_def` (naming both versions
in the log); a config with no `_schema` stamp is tolerated.

## Bilingual: pack-mode emit (polaris2 wave-2)

`json_source` is **bilingual** (docs/new_gen/10 — Gate P1). Set the `pack_mode`
config flag (**default `false`**) and, on a host that publishes the `xi.pack@1`
door, each `process()` emission builds a sealed [Pack](../../docs/new_gen/07-uniform-keyed-buffer-plane.md)
through the host emit door — exactly as `mock_camera` does — **instead of**
returning a Record. With `pack_mode` off (every existing project), the Record
path below is **byte-for-byte unchanged**.

```jsonc
// config wrapper (set_def): flip the flag alongside the document
{ "pack_mode": true, "data": { "n": 42, "roi": { "x": 1, "pts": [3,4,5] } } }
```

**Mapping rule — JSON document → pack entries.** The (patched) document must be
a JSON **object** at its root; each top-level field becomes one pack entry:

| Top-level JSON value | Pack entry |
|---|---|
| number (integer) | canonical `i64` |
| number (real) | canonical `f64` |
| string | canonical `str` |
| boolean | `i64` `0`/`1` (the pack scalar plane has no bool tag) |
| `null` | **skipped** |
| object / array (**nested**) | **one** `mp` entry — the value encoded to canonical msgpack and re-proven through the ingress edge (see below) |

Plus a leading **`seq`** `i64` entry — a per-emit counter that mirrors
`mock_camera`. `seq` is **reserved**: a user field literally named `seq` is
shadowed by the counter. Nesting is msgpack's job (doc 07 §D3): a nested object
or array rides as **one** opaque `mp` entry that a generic walker recurses into;
it is **not** flattened into dotted keys.

**Foreign input crosses the domain edge.** The user's JSON is FOREIGN, untrusted
input, so a nested value never reaches the pack plane raw: it is encoded to
msgpack and passed through `xi::ingress::canonicalize_entry`
([`xi_ingress.hpp`](../../backend/include/xi/xi_ingress.hpp)) — the ONLY
sanctioned foreign-bytes→pack path — which validates structure (bounded depth,
declared-vs-actual lengths, no trailing bytes), rejects duplicate/non-string map
keys, and normalizes to the canonical max-width profile in one pass (doc 07
"Ingress"). The JSON→msgpack walk is itself depth-bounded so a depth-bomb
document cannot overflow the plugin's stack before the edge runs.

**Fail loud.** Hostile or malformed input is never silently dropped or partially
emitted — it produces a sealed **`$fault`** pack (the pilot convention: a
contract failure is still a normal pack the consumer routes to a verdict),
carrying `seq` + the reason code:

| Condition | `$fault` code |
|---|---|
| nested value deeper than the ingress limit (`kDefaultMaxDepth`) | `depth-exceeded` |
| nested entry over the size cap (8 MiB) | `oversized` |
| other ingress rejection | the `xi::mp` status string (`length-overflow`, `duplicate-key`, …) |
| document root is not a JSON object | `wrong_type` |

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

`tests/test_json_source_pack.cpp` (`json_source_pack_test`) is the bilingual
proof against the **real DLL** through the `xi.pack@1` door: `pack_mode` off
leaves the Record path untouched (zero packs on the bus); on, the document's
scalars land as canonical entries, a nested object rides as one
ingress-canonicalized `mp` entry that decodes back, and a leading `seq` counter
advances per emit; a depth-bomb / non-object document is rejected loudly with a
`$fault` pack. Pooled-handle balance is asserted across the run. It follows
`pack_pilot_test`'s host-mock pattern (`make_host_api` + `install_pack_abi` +
`install_trigger_hook`).
