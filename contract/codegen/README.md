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
- **`_schema.gen.h`** — the `xi::FrameSchema<Derived>` CRTP keyset (enum slots +
  `keys` array) for the offset-accessor frame path (`xi_frame.hpp` /
  `docs/new_gen/07`). Flat top-level scalar/image keys only — arrays are not
  slots. `slot_of("k")` is a compile-time constant; keyset drift is a build error.
- **`_io.gen.h`** — the `Input`/`Output` (operator) or `Config`/`Command`/`Frame`
  (source) builder+extractor classes, byte-for-byte the call surface the plugin
  and its tests use. Compiles down to `xi::Record`/`xi::Json` set/get — nothing
  new crosses the ABI.
- **`.gen.ts` / `_gen.py`** — typed views over the JSON-carried keys (images are
  omitted: they ride the Record image bag, not the JSON).
- **`_keys.md`** — a docs keys-table fragment (so review 11's "README shows a
  retired key" rot becomes impossible — the table is generated).

## The equivalence proof (the wave-3 exit criterion)

Two ctests, both label `contract`, wired in `backend/CMakeLists.txt`:

1. **`codegen_equiv`** (`check_equiv.py`, pure stdlib) — regenerates into a tmp
   dir and (a) byte-compares against the committed `generated/plugins/` (stale =
   fail); (c) extracts the `{kName -> "value"}` + `kSchemaVersion` set from the
   generated `_keys.gen.h`, the decl, **and** the hand-written
   `plugins/<p>/<p>_keys.h`, and fails if any of the three disagree — that is the
   drift guard both directions; (e) **WARNs** (non-fatal) for every plugin that
   ships a hand-written `*_keys.h` with no decl (the coverage ratchet — the
   covered set only grows). Today it warns for `config_swap_probe`, `json_source`,
   `synced_stereo`.
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

## Swap-in plan (for the integration lead — do NOT execute in this branch)

The hand-written `plugins/<p>/{<p>_keys.h,<p>_io.h}` are **left in place** here on
purpose: the wave-2 pilot (frame door) is concurrently editing those plugins.
When the pilot has landed and the accessor convention is stable, the integration
lead performs the swap per plugin, at wave-2 exit:

1. **Regenerate** the artifacts from the decls:
   `python contract/codegen/gen_contract.py`
   (and confirm `ctest -R codegen_equiv` is green — generated ≡ hand-written).
2. **Point the includes at the generated headers.** Either copy the two
   `generated/plugins/<p>_{keys,io}.gen.h` over the hand-written
   `plugins/<p>/<p>_{keys,io}.h` (keeping the include *names* the `.cpp`/tests
   use), or add `generated/plugins/` to the plugin's include path and switch the
   `#include "<p>_keys.h"` lines to `"<p>_keys.gen.h"`. No call-site edit is
   needed — the surface is identical (that is exactly what `codegen_equiv` and
   `codegen_equiv_compile` prove).
3. **Delete the hand-written headers** once the plugin builds against the
   generated ones, and add the plugin's `_schema.gen.h` where the frame path is
   adopted. From then the decl is the single source of truth; `codegen_equiv`
   keeps it and the (now generated) headers in lockstep, and the config-UI forms,
   Python typing, and generated docs from the same decl light up (stage-2 tail).

Regenerating is idempotent and deterministic, so step 1 can be re-run any time;
the `codegen_equiv` stale-check guarantees a committed artifact is never behind
its decl.
```
python contract/codegen/gen_contract.py            # regenerate all artifacts
python contract/codegen/gen_contract.py --check    # verify committed == regenerated
python contract/codegen/check_equiv.py             # the full equivalence + drift gate
```
