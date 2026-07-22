# `contract/codegen/` — the plugin data-contract generator

Status: **wave-3 seed** of [`docs/new_gen/02-plugin-data-contract.md`](../../docs/new_gen/02-plugin-data-contract.md)
stage 2, dispatched by [`docs/new_gen/08-polaris2-main-plan.md`](../../docs/new_gen/08-polaris2-main-plan.md)
Wave 3. It turns **one declaration per plugin** into the generated typed-view
artifacts, and proves those artifacts are a **drop-in** for the hand-written
`_keys.h`/`_io.h` the plugins ship today (guard 4: "stage 2 is a swap").

> **THE CUT (v12):** the `_io.gen.h`/`_schema.gen.h` halves (and the compiled
> `codegen_equiv_compile` gate) were retired with `xi::Record`/`xi_record_schema.hpp`;
> only the ABI-neutral `_keys.gen.h` (+ `.gen.ts`/`_gen.py`/`_keys.md`) are still
> generated, and `codegen_equiv` gates on keys alone. Sections below describing
> the io/schema halves are historical.

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
  "config": [ { "key", "type", "readonly"?,
                "raw_json"?, "default"? }, ... ],   // raw_json: the value is
                                       // pre-serialized JSON TEXT spliced
                                       // verbatim (json_source's open "data");
                                       // default = the member's initial text.
                                       // A Config with raw fields emits setters
                                       // ONLY for those (concat dump()).
  "commands": { "selector_key", "value_key",
                "list": [ { "name", "doc"? },
                          { "name", "params": [ {"key","arg","type",
                                                 "default"?,        // C++ default arg (fire(int n = 1))
                                                 "raw_json"?} ] } ] }, // spliced JSON text; must be the ONLY param
  "output_frame": [ { "key", "type": "image", "accessor", "has_accessor" } ],

  // source extras (the codegen-gap-#2 families, all fixed shapes):
  "replies": [ { "class": "Status", "of_command": "get_status",   // typed reader over an
                 "fields": [ { "key", "type": int|double|bool|string,  // exchange() REPLY string
                               "accessor"?,          // has_staged() over the "staged" key
                               "cast"?, "doc"? } ] } ],  // whitelisted C++ cast (e.g. "long long"
                                                         // over the double read) — REPLY_CASTS
  "patch_builder": { "class": "Patch", "path_key", "value_key", "list_key",
                     "single_accessor"?, "batch_accessor"?, "doc"? },
                                       // the fixed {key,value} single / {patches:[...]}
                                       // batch raw-JSON process()-input shape
  "frame_composites": [ { "accessor": "has_both", "all_of": ["left","right"], "doc"? } ]
                                       // derived AND of the per-image has_* checks
}
```

Every decl is checked by the **subset validator** (`_validate_decl`, run on
every load by `gen_contract.py` AND `check_equiv.py`): unknown sections or
attributes are errors, `raw_json`/`cast`/`patch_builder` are fixed whitelisted
shapes — the decl language stays a constrained subset, not a template engine.

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
- **`_io.gen.h`** — the `Input`/`Output` (operator) or `Config`/`Command`/
  `Patch`/reply-extractor/`Frame` (source) builder+extractor classes,
  byte-for-byte the call surface the plugin and its tests use. Compiles down to
  `xi::Record`/`xi::Json` set/get (raw-JSON shapes concatenate pre-serialized
  text, exactly like the hand-written originals) — nothing new crosses the ABI.
  The `Frame` class is emitted only when the frame carries at least one image
  (an accessor-less Frame is noise). SKIPPED when the decl sets
  `"handwritten_io": true` — the escape hatch for a surface the subset cannot
  express; since the codegen-gap-#2 families landed NO in-tree decl uses it.
  See "Coverage" below.
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
   must emit exactly the decl's key set. If a hand-written `toolbox/<p>/<p>_keys.h`
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
so the swap is done. **Mechanism chosen: include-path add** — `toolbox/CMakeLists.txt`
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

## Coverage

All in-tree contract plugins carry a decl and are now **full swaps** (coverage
ratchet **empty**): `_keys.gen.h` AND `_io.gen.h` replaced the hand-written pair
and the hand-written headers are deleted. The compiled `codegen_equiv_compile`
gate drives every plugin's generated builder/extractor call sites.

The wave-2 "codegen gap" (the keys-only trio) was closed by the polaris2
**codegen-gap-#2 extension** — four new decl families, each a fixed shape the
generator emits verbatim (validated by the subset validator, no open templating):

- **`replies`** — a typed reader class over an exchange() reply *string*
  (`xi::Json::parse` + `valid()` + `as_*` getters, per-field `accessor` renames
  for `has_*` semantics, and a whitelisted `cast` for bespoke reads). Closed
  `config_swap_probe` (`Status` with `active()`/`has_staged()`/
  `proc() -> long long`).
- **raw-JSON splicing** — `"raw_json": true` on a config field (with a
  `default` initial text) or on a command's single param: the value is
  pre-serialized JSON TEXT concatenated verbatim, the one deliberately
  string-shaped corner. Closed `json_source`'s open `Config.data()` +
  `set_data`.
- **`patch_builder`** — the fixed `{key,value}` single / `{patches:[...]}`
  batch process()-input shape. Closed `json_source`'s `Patch`.
- **param `default` + `frame_composites`** — C++ default args on command
  factories and derived AND-composites over per-image `has_*` checks. Closed
  `synced_stereo`'s `fire(int n = 1)` + `has_both()`.

Idiom notes from the swap (compile+behavior equivalence proven by
`codegen_equiv_compile`; the emitted JSON bytes are identical): generated
no-param commands always route through the private `cmd_` helper, `.set` chains
are single-line, hand-written column alignment is not reproduced, commands
follow decl order, and a decl'd settable config key always gets a setter
(historical example: synced_stereo's typed `Config.pack_mode(bool)` was
generated although the hand-written view omitted it — additive, no call-site
impact; both that knob and the typed `_io` views have since been retired).

`"handwritten_io": true` remains supported as the escape hatch for a future
plugin whose surface genuinely exceeds the subset; nothing in-tree uses it.
```
python contract/codegen/gen_contract.py            # regenerate all artifacts
python contract/codegen/gen_contract.py --check    # verify committed == regenerated
python contract/codegen/check_equiv.py             # the full equivalence + drift gate
```
