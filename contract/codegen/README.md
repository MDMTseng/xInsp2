# `contract/codegen/` — the plugin data-contract generator

Status: **wave-3 seed** of [`docs/new_gen/02-plugin-data-contract.md`](../../docs/new_gen/02-plugin-data-contract.md)
stage 2, dispatched by [`docs/new_gen/08-polaris2-main-plan.md`](../../docs/new_gen/08-polaris2-main-plan.md)
Wave 3. It turns **one declaration per plugin** into the generated typed-view
artifacts, and proves those artifacts are a **drop-in** for the hand-written
`_keys.h`/`_io.h` the plugins ship today (guard 4: "stage 2 is a swap").

## The two generators here

| Script | Input | Output |
|---|---|---|
| `gen_types.py` (the original probe) | `../schemas/run-outcome.schema.json` | `generated/run_outcome.generated.{ts,py}` — the wire-payload TS/Py probe |
| `gen_contract.py` (this seed) | `../plugins/<plugin>.decl.json` | `generated/plugins/<plugin>_{keys,schema,io}.gen.h` + `.gen.ts` + `_gen.py` + `_keys.md` |

`gen_contract.py` extends `gen_types.py`'s conventions: the same `GENERATED …
DO NOT EDIT` banner, the same scalar type maps, deterministic output (declaration
order, no timestamps) so a regenerate is a no-op diff.

## The declaration format (`../plugins/<plugin>.decl.json`)

ONE file per plugin, **transcribed** from today's hand-written headers (the
headers are the current truth — a decl mirrors them, it does not redesign them).
It stays in the constrained field family the repo governs (typed fields,
required/optional, ranges, nested shapes for arrays — no combinators/conditionals):

```
{
  "plugin": "blob_analysis",          // dll/base name
  "namespace": "xi::blob",            // the C++ namespace the headers emit into
  "schema_version": 1,               // -> kSchemaVersion (guard 3 skew stamp)
  "role": "operator" | "source",

  // operator: process() input Record + output Record
  "inputs":  [ { "key", "type": int|double|bool|string|image,
                 "required", "channels"?, "default"?, "doc"? }, ... ],
  "outputs": [ { "key", "type" },
               { "key", "type": "array", "item_accessor", "size_accessor",
                 "item_class", "fields": [ ...nested, arrays may recurse... ] } ],

  // source: config (get_def/set_def) + commands (exchange) + emitted frame
  "config": [ { "key", "type", "readonly"? }, ... ],
  "commands": { "selector_key", "value_key",
                "list": [ { "name" }, { "name", "params": [ {"key","arg","type"} ] } ] },
  "output_frame": [ { "key", "type": "image", "accessor", "has_accessor" } ]
}
```

`item_accessor` / `size_accessor` / `item_class` are carried explicitly because
they are **not** mechanically derivable (the hand-written blob extractor names
the `blobs` array's item accessor `blob()` and its point class `Point`, not
`Contour`). The generator reproduces those names exactly.

### What each artifact is

- **`_keys.gen.h`** — guard 1: every key named once as `inline constexpr const
  char* kName = "value";` in `namespace <ns>::keys`, plus `kSchemaVersion`. The
  constant NAMES + VALUES match the hand-written `_keys.h` exactly (`min_area`
  → `kMinArea`), so the plugin `.cpp`, the builder, and the extractor all keep
  compiling against it.
- **`_schema.gen.h`** — the `xi::PackSchema<Derived>` CRTP keyset (enum slots +
  `keys` array) for the offset-accessor pack path (`xi_pack.hpp` /
  `docs/new_gen/07`). Flat top-level scalar/image keys only — arrays are not
  slots. `slot_of("k")` is a compile-time constant; keyset drift is a build error.
  SKIPPED when the plugin declares no flat frame slots (a source with no
  `output_frame`): an empty schema is noise nothing adopts.
- **`_io.gen.h`** — the `Input`/`Output` (operator) or `Config`/`Command`/`Frame`
  (source) builder+extractor classes, byte-for-byte the call surface the plugin
  and its tests use. Compiles down to `xi::Record`/`xi::Json` set/get — nothing
  new crosses the ABI. SKIPPED when the decl sets `"handwritten_io": true` — some
  plugins' control surface is outside the constrained typed-field family (an
  open-schema raw-JSON builder, a reply/status extractor, a derived convenience
  accessor); the generator owns their KEY contract but their `_io.h` stays
  hand-written. See "Coverage & the codegen gap" below.
- **`.gen.ts` / `_gen.py`** — typed views over the JSON-carried keys (images are
  omitted: they ride the Record image bag, not the JSON).
- **`_keys.md`** — a docs keys-table fragment (so review 11's "README shows a
  retired key" rot becomes impossible — the table is generated).

## The equivalence proof (the wave-3 exit criterion)

Two ctests, both label `contract`, wired in `backend/CMakeLists.txt`:

1. **`codegen_equiv`** (`check_equiv.py`, pure stdlib) — regenerates into a tmp
   dir and (a) byte-compares against the committed `generated/plugins/` (stale =
   fail); (c) extracts the `{kName -> "value"}` + `kSchemaVersion` set from the
   generated `_keys.gen.h` and the decl and fails if they disagree — the generator
   must emit exactly the decl's key set. If a hand-written `plugins/<p>/<p>_keys.h`
   is **still present** (a plugin declared but not yet swapped) it is ALSO compared,
   so the pending swap is proven a true drop-in; once swapped, that header is gone
   and the decl↔generated leg is the whole proof (an absent hand-written header is
   the swapped state, not a failure). (e) **WARNs** (non-fatal) for every plugin
   that ships a hand-written `*_keys.h` with no decl (the coverage ratchet — the
   covered set only grows). All five originally-covered/warned plugins are now
   swapped, so the WARN list is **empty**; the mechanism stays for future plugins.
2. **`codegen_equiv_compile`** (`equiv/test_codegen_equiv.cpp`) — a custom command
   regenerates the headers into the build tree, then this TU `#include`s the
   GENERATED headers in place of the hand-written ones and drives the same
   builder/extractor call sites as the plugin tests. A renamed constant or
   changed signature stops it compiling; the runtime asserts prove the generated
   builders/extractors move the same bytes. DLL-free (output Records are rebuilt
   from JSON exactly as the plugin tests do after the C-ABI hop).

Together: `codegen_equiv` proves *generated ≡ hand-written ≡ decl* at the key
level; `codegen_equiv_compile` proves the generated headers *satisfy the real
call sites* at compile+run time. That is stage-2's "zero call-site changes"
promise, mechanised.

## Swap-in (EXECUTED — wave-2 exit, docs/new_gen/08 Wave 3)

The wave-2 pilot (pack door) has landed and the accessor convention is stable,
so the swap is done. **Mechanism chosen: include-path add** — `plugins/CMakeLists.txt`
puts `contract/codegen/generated/plugins/` on the plugin build's include path
(directory scope, so plugin DLLs and their contract-test exes both see it) and
each consumer's `#include "<p>_keys.h"` / `"<p>_io.h"` became `"<p>_keys.gen.h"` /
`"<p>_io.gen.h"`. The ONE committed generated set is the single source of truth,
and its regenerate-diff shows in git at exactly the path `codegen_equiv` already
byte-checks. (The alternative — copying the `.gen.h` back into each plugin dir via
a build-time custom command — was rejected: it either duplicates a committed copy
per plugin, a second drift surface, or makes the copy build-time-only, hiding the
regenerate-diff from git. The include-path add has neither problem and reuses the
existing `../backend/include` cross-tree reference style the plugin build already
uses.) The `#include` line is a *mechanism* edit, not a *call-site* edit — every
builder/extractor call site (`Input().threshold(…)`, `keys::kFoo`, `Frame{…}`,
`out.blob(i).area()`) compiled **unchanged**, which is stage-2's promise, proven.

Per plugin the swap was: regenerate → point includes at the generated headers →
delete the hand-written headers once every consumer (plugin `.cpp`, tests,
`pack_pilot_test`, the PackSchema usage) built against the generated set with
zero call-site edits. Regeneration is idempotent/deterministic (`--check` guards a
stale commit), so the decl is now the single source of truth; `codegen_equiv`
keeps it and the generated headers in lockstep.

## Coverage & the codegen gap

All in-tree contract plugins now carry a decl and consume the generated
`_keys.gen.h` (coverage ratchet **empty**). Two levels of swap resulted:

- **Full swap** — `blob_analysis`, `mock_camera`: both `_keys.gen.h` AND
  `_io.gen.h` replace the hand-written pair; the hand-written headers are deleted.
  These are the wave-2 pilot pair; the compiled `codegen_equiv_compile` gate drives
  their generated builder/extractor call sites.
- **Keys-only swap** — `config_swap_probe`, `json_source`, `synced_stereo`: their
  `_keys.gen.h` is a true drop-in and replaces the hand-written `_keys.h`, but their
  `_io.h` stays **hand-written** (their decl sets `"handwritten_io": true`, which
  suppresses `_io.gen.h`). Their control surface is outside the constrained
  typed-field family the generator emits, so generating the I/O would require either
  redesigning the generator past its governance or editing the tests' call sites —
  the stage-2 rule is to STOP and record the gap, not force it. The specific gaps:
  - `config_swap_probe` — a `get_status` **reply/status extractor** (`Status` with
    `valid()`/`active()`/`has_staged()`/`proc()`), a class family the generator has
    no concept of (it emits `Frame`, not a typed reply reader).
  - `json_source` — an **open-schema** control surface *by design*: `Config.data()`
    splices raw user JSON, `set_data` carries arbitrary JSON, and `Patch::single/
    batch` is open-valued. This is explicitly not typed-field-declarable.
  - `synced_stereo` — a derived `has_both()` convenience and a defaulted
    `fire(int n = 1)`; small, but not what the generator emits verbatim.

  Closing these is a future generator extension (a declarative reply-extractor
  family; a per-command default; a combined has-accessor). Until then the decl owns
  their keys and the `codegen_equiv` drift gate guards those; the bespoke `_io.h`
  is a thin veneer over the generated `keys::` constants.
```
python contract/codegen/gen_contract.py            # regenerate all artifacts
python contract/codegen/gen_contract.py --check    # verify committed == regenerated
python contract/codegen/check_equiv.py             # the full equivalence + drift gate
```
