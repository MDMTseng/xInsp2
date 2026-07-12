# json_source

Emits a configurable JSON document as a sealed `xi.pack@1` pack. The user edits
the JSON via the plugin's GUI; on every tick the stored document is emitted
through the host pack door. Useful for injecting test fixtures, configuration,
or manual data into a pipeline.

## Open output, typed control surface

The JSON this plugin emits is **user-defined**, so its output data plane is an
**open schema** by design (docs/new_gen/02-plugin-data-contract.md — intrinsically
generic plugins declare open schemas). There is deliberately **no typed Output
extractor**: the produced pack is whatever the user authored.

What *is* a fixed, declarable vocabulary is the plugin's **control surface** — its
commands and its config wrapper. Those key names follow the plugin data contract:

## Keys — one source of truth

Every control key is declared **once** in
[`../../contract/plugins/json_source.decl.json`](../../contract/plugins/json_source.decl.json),
from which `contract/codegen/gen_contract.py` generates `json_source_keys.gen.h`
(the `keys::` constants the plugin's readers compile from) plus the TS/Py
typings and the docs keys table. The config and `set_data` splice raw user
JSON — an OPEN control surface by design — declared through the decl's
constrained raw-JSON shapes.

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `data`    | json | the stored user document (wrapped: `{ "data": … }`) |
| output  | `seq`     | int  | per-emit counter (mirrors mock_camera) |
| command | `command` | string | `set_data` / `reset` / `get_status` |
| command | `value`   | json | **required** for `set_data` (the new document) |

Schema version: `xi::json_source::kSchemaVersion` (currently **1**). A config
built against a different version is rejected by `set_def` (naming both versions
in the log); a config with no `_schema` stamp is tolerated.

(v12 / THE CUT: the wave-2 bilingual `pack_mode` knob and the per-emit Record
input patches are gone with the Record plane — the sealed pack is the sole emit
currency, and document edits go through `set_data`/`set_def`.)

## Pack emit — JSON document → pack entries

The stored document must be a JSON **object** at its root; each top-level field
becomes one pack entry:

| Top-level JSON value | Pack entry |
|---|---|
| number (integer) | canonical `i64` |
| number (real) | canonical `f64` |
| string | canonical `str` |
| boolean | canonical `bool` (tag `XI_PACK_TAG_BOOL`, the 0xc2/0xc3 byte) |
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
host.set_def(src, R"({"data":{"roi":[0,0,64,64]}})");
host.exchange(src, R"({"command":"set_data","value":{"n":3}})");
// Each tick emits the stored document as ONE sealed pack (entries {seq, n}).
```

## Failure shape

`set_data` with no `value` fails loud rather than silently keeping the old JSON:

```json
{ "error": "missing_input", "key": "value", "expected_type": "json" }
```

(`error` uses the shared `xi::contract` reason codes.)

## Tests

`tests/test_json_source_pack.cpp` (`json_source_pack_test`) is the proof
against the **real DLL** through the `xi.pack@1` door: the document's scalars
land as canonical entries, a nested object rides as one ingress-canonicalized
`mp` entry that decodes back, and a leading `seq` counter advances per emit; a
depth-bomb / non-object document is rejected loudly with a `$fault` pack.
Pooled-handle balance is asserted across the run. It follows
`pack_pilot_test`'s host-mock pattern (`make_host_api` + `install_pack_abi`).
Run via `ctest --test-dir plugins/build -C Release -R json_source_pack_test`.
