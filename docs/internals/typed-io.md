# Typed I/O + the contract codegen — the mechanics

**Shipped design-of-record (ABI v12).** What the plugin data contract generates
now, and how compile-checked keyed reads work on the sealed-pack data plane.

> **THE CUT (v12).** The typed **Record** I/O layer this page used to describe —
> nominal types over `xi::Record`, per-plugin `io.hpp` extractor/constructor
> facades, `xi::Typed` shallow views with write-through + COW, and `$na`
> propagation — was **retired at THE CUT together with `xi::Record`**. The
> generated `<plugin>_io.gen.h` (typed Record I/O views) and
> `<plugin>_schema.gen.h` (`xi_record_schema` keysets) halves went with it. What
> survives is the **ABI-neutral key contract** — key *names* and *values* — plus
> the typed-keyed reads the **pack plane** provides. The deleted layer is kept as
> labeled history at the end ([What THE CUT retired](#what-the-cut-retired-record-typed-io)).

## What the contract generator emits now — `contract/codegen/gen_contract.py`

One declaration file (`contract/plugins/<plugin>.decl.json`) is the single source
of truth; the generator turns it into these artifacts (deterministic — declaration
order, no timestamps, so a regenerate is a no-op diff; `--check` is the staleness
ctest):

| Artifact | What it is |
|---|---|
| `<plugin>_keys.gen.h` | The key-constants contract (guard 1): `namespace keys { inline constexpr const char* kMinArea = "min_area"; … }` + `kSchemaVersion`. Every string key named exactly once; the builder, any extractor, and the plugin's own reader all compile from it. |
| `<plugin>.gen.ts` | TypeScript interfaces over the JSON-carried keys (for the webUI). |
| `<plugin>_gen.py` | Python `TypedDict`s over the JSON-carried keys. |
| `<plugin>_keys.md` | A docs keys-table fragment. |

The `_io.gen.h` and `_schema.gen.h` generators lived in this file until THE CUT;
the source now carries only the retirement note where they were (and `render()`
lost the conditional `handwritten_io` / empty-frame-slot skip logic those
artifacts needed). **Only the ABI-neutral halves remain generated.**

The decl still **declares and validates** the full surface — `inputs`, `outputs`,
`config`, `commands`, `replies`, `output_frame` — because those declarations still
mint key constants (that is what `_keys.gen.h` collects, via `collect_keys`). What
changed is that the *typed views* over those keys are no longer code-generated: a
reply field, for instance, is still declared and its key still rides `_keys.gen.h`,
but the typed reply **extractor** went with `_io.gen.h`. The `_validate_decl`
constrained-subset checker (unknown sections/attributes are errors; the escape
hatches — `raw_json` splice, whitelisted reply `cast`, `patch_builder` — are fixed
shapes) is unchanged, so a decl the generator would mis-render is still refused at
load by both `gen_contract.py` and `check_equiv.py`.

## Typed, compile-checked reads on the pack plane

Keys are still named exactly once and reads are still drift-proof — the mechanism
moved from typed Record views to the sealed pack:

- **By key constant.** A plugin door reads/writes with its `_keys.gen.h`
  constants through `xi::PackIn`/`xi::PackOut` (`in.i64(keys::kThresh)`,
  `out.image(keys::kDst, …)`), and a script through `xi::ScriptPack`
  (`f.get_i64(...)`) — no string literals at call sites, no schema in the core.
  Every typed read returns `std::optional`, so absence is explicit rather than a
  silent default. (Since ④A the inline payload is the canonical msgpack value —
  memory == wire — so a typed read skips the fixed-width header at a known offset
  and serialization is a verbatim splice; see [`pack-plane.md`](./pack-plane.md).)
- **By declared keyset (compile-checked slots).** `xi::ScriptTypedPack<Schema>`
  (`xi_use.hpp`) wraps a `ScriptPack` with a schema of key **slots**:
  `get_i64<Schema::kSeq>()` resolves the slot to its key constant at compile time,
  so a mistyped slot is a build error, not a runtime miss (`qa/qa_pack_walk`
  is the reference). This is the pack analogue of the retired declared-keyset
  Record views. (Note: `ScriptTypedPack` is key-based over the opaque
  `xi_pack_v1` ABI and needs only `Schema::keys`; it is unrelated to the
  in-process `TypedPack<Schema>`/`PackSchema` offset container, which was
  deleted 2026-07-11 — commit `cba51fe` — leaving `Pack`/`PackBuilder` as the
  one in-process container.)
- **Producer-agnostic walk.** A sink that has no producer schema enumerates an
  unknown pack with `count()` + `key_at`/`tag_at` (or the `for_each(key, tag)`
  sugar) — the pack's self-description (doc 07 §2).

## Provenance + fault ride the pack (same key *names*, pack shapes)

The `$src` / `$prov` provenance convention and the fail-loud discipline survived
the currency swap with the same reserved key **names** but pack-native shapes:
`$src` is the immediate producer (instance name), `$prov` a `/`-joined hop chain
(oldest→newest), stamped by the pack-door glue **before seal** (an immutable pack
means new lineage is a new pack); a contract failure is a normal sealed pack
carrying `$fault` (never a null handle), and the host funnel short-circuits a fault
input by minting a propagated fault pack — the pack mirror of the retired
`use("x").process(na)` returning NA without running the plugin. One home for the
reserved `$`-keys and the shared helpers: `xi/xi_pack_contract.hpp`. Full
semantics: [`pack-plane.md`](./pack-plane.md).

## See also

- `contract/codegen/gen_contract.py` — the generator; `contract/codegen/check_equiv.py`
  (the `codegen_equiv` ctest) — the drop-in-equivalence proof vs the hand-written
  `_keys.h`.
- [`data-layer.md`](./data-layer.md) — the sealed-pack container + refcount mechanics.
- [`pack-plane.md`](./pack-plane.md) — the pack contract, fault semantics and ingress.
- `xi/xi_abi.hpp` (`PackIn`/`PackOut`), `xi/xi_use.hpp` (`ScriptPack`/`ScriptTypedPack`).

---

## What THE CUT retired (Record typed I/O) — historical

> Everything below is **retired at THE CUT (v12)** with `xi::Record`. It is kept
> as a tombstone for old links; none of it exists in the shipped SDK. The pack
> plane above covers the same need (typed, drift-proof, self-describing keyed I/O)
> without a schema in the core.

- **Nominal types over Record.** `Number / Point / Vec2..4 / Line / Arc / Pose /
  Roi / Mat2..4 / Region` (`xi/xi_types.hpp`) were each *just a name* over a
  generic `xi::Record` — no fields enforced, payload schema-less JSON. The
  compiler stopped you wiring a `Line` into a `Pose` input, but `process()` stayed
  untyped (`Record` only) and the types lived purely in the wiring layer.
- **`xi::Typed` shallow views + write-through + COW.** A `Typed` was OWNED or a
  VIEW sharing a parent's `shared_ptr<Record>`; extractors handed out views so
  pulling N nested values cost no duplication. `set()` was write-through (NumPy
  semantics), routed through `prepare_write_()` → `Record::materialize_unfrozen()`
  so a write to a *frozen/shared* Record (e.g. `current_trigger().meta()`) copied
  first. All of this depended on the Record doc layer's COW, which is gone.
- **Per-plugin `io.hpp` facades.** Each plugin shipped a header-only `io.hpp` the
  script `#include`d — a `<plugin>_io.extract(rec)` extractor (one getter per
  output port) and a `<plugin>_io.build()` constructor (one setter per input
  port), both **total** (a missing field yielded NA, never threw). These were the
  `_io.gen.h` half of the codegen, retired above.
- **NA propagation (`$na`).** Validation happened at the compute boundary inside
  `process()`: a whole-Record NA short-circuited `use("x").process(na)`;
  `xi::require(in, {…})` bailed to NA in one line; `xi::Record::na("reason")` was
  an empty output carrying a reason that flowed to the end of the pipeline. The
  pack plane's `$fault` (a normal sealed pack, `is_fault`/`propagate_fault`) is the
  direct replacement.
- **Iconic `Region`.** The first nominal type whose data was an image — a binary
  mask (CV_8U) under image key `"mask"` with `w`/`h` mirrored into JSON. On the
  pack plane a mask is simply an image entry beside its scalar metadata in the same
  sealed pack.
