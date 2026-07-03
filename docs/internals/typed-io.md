# Typed I/O + NA — the mechanics

**Shipped design-of-record (phases 1–3; the wiring UI is deferred — see roadmap).**
How nominal types + per-plugin `io.hpp` facades + NA propagation turn plugin
wiring from hand-written JSON juggling into a straight line, **without putting a
schema in the core**. The type *names* + Image bag the wiring author sees are in
[`../reference/data-types.md`](../reference/data-types.md); this is how they work.

## Nominal types are just names over Record

`Number / Point / Vec2/3/4 / Line / Arc / Pose / Roi / Mat2/3/4 / Region`
(`xi/xi_types.hpp`) are each *just a name* over a generic `xi::Record` — no fields
enforced, payload still schema-less JSON. A wrapper is a lightweight handle (holds
a Record), carries schema-less accessors (`pose.angle()` reads `rec["angle"]`, NA
if absent), and **can be NA**. The compiler stops you wiring a `Line` into a
`Pose` input, but the data never leaves generic Record and `process()` stays
untyped (`Record` only). Types live purely in the wiring layer; plugin/toolbox
authors define their own (e.g. a toolbox `MatchResult`) in their own `io.hpp`.

### Shallow views, write-through, COW

A `xi::Typed` is OWNED (holds its own Record) or a VIEW (shares a parent's
`shared_ptr<Record>` + points a yyjson value at a sub-node — no copy). Extractors
hand out views, so pulling N nested values costs no duplication; the one
materialising copy is `record()`, when you embed a value into a constructor input.
`set()` is **write-through by design** (like a NumPy view) — it mutates the node in
the shared tree; `.clone()` first when you want an independent copy. (Note: the doc
layer's COW means a write to a *frozen/shared* Record copies first — see
[`data-layer.md`](./data-layer.md); a freshly-extracted owned Record is writable.)

`xi::Typed`/`Field` writes (`set()`, `roi["x"] = …`, `MatN::set`) honour that same
copy-on-write boundary: they route through `Typed::prepare_write_()`, which calls
`Record::materialize_unfrozen()` (the public hook over `Record::cow_`) **before**
touching the tree. So writing through a Typed whose Record is **frozen** — shared,
borrowed, or registry-managed across the ABI, e.g. `current_trigger().meta()` — no
longer mutates the doc the other side still reads; it copies first. The fast path is
unchanged: a non-frozen write is one relaxed flag-load + a pointer re-seat, **no
copy**. Two cases:

- **OWNED** Typed (`node_ == root_->json()`): COWs the whole Record and re-seats
  `node_` at the (possibly new) root. This is the common reachable path
  (`xi::Roi(current_trigger().meta())` is an OWNED Typed via the `Typed(Record)`
  ctor).
- **VIEW into a frozen doc** (an interior sub-node of a cross-ABI shared tree): the
  interior node can't be cheaply re-resolved into a copy, so the write **detaches**
  — the view becomes an OWNED standalone copy of just that sub-node (an implicit
  `.clone()`). The write lands in the private copy; the frozen original is untouched.
  A *non-frozen* view still writes through in place (the NumPy semantics above) — the
  detach only happens when the original is frozen, where in-place mutation would
  corrupt the other side.

## Per-plugin `io.hpp` — extractor + constructor facades

Each plugin ships a header-only `io.hpp` (alongside `plugin.json`) the script
`#include`s; it mirrors the manifest:

```cpp
auto e = matcher_io::extract(rec);     // one getter per output port
e.count();                             // -> Number
e.orientation(i);                      // -> xi::Pose (i-th, lazy view)

auto in = line_fit_io::build()         // one setter per input port
            .current(e.orientation(k))
            .baseline(/* from config */)
            .build();                  // -> xi::Record (complete)
```

**Extractors and constructors are TOTAL — they never fail.** A missing field
yields an empty / NA value, not an exception. The wiring code is a straight line:
no null-checks, no try/catch.

## NA propagation — validate at the compute boundary

Validation happens where the data is *used* — inside the plugin's `process()`:
- If the **whole input Record is NA**, the framework short-circuits:
  `use("x").process(na)` returns NA without running the plugin.
- If **some fields are missing**, the plugin bails to NA in one line:
  `if (auto na = xi::require(in, {"current","baseline"})) return *na;`
- A plugin that can't proceed returns `xi::Record::na("reason")` — an empty output
  carrying the reason. The next stage's input is then NA, so it short-circuits too:
  **NA flows to the end of the pipeline carrying its reason, with no defensive code
  in between.**

NA is a first-class Record concept, a reserved `"$na"` key so it crosses the
`process()` ABI + WS unchanged:

```cpp
xi::Record::na("reason")   // { "$na": "reason" }, no images
rec.is_na(); rec.na_reason();
```

This extends run-level NA (`xi::result` code 0) down to the Record level so it
flows *between* stages.

> **Pack-plane mirror.** The v3 Pack plane has the same shape with different
> vocabulary: a contract failure is a normal sealed pack carrying a `$fault`
> reason code (never a null handle), and the host funnel short-circuits a fault
> input by minting a new fault pack with the hop appended — the pack mirror of
> `use("x").process(na)` returning NA without running the plugin. The contract
> (reserved `$fault`/`$fault_key`/`$fault_detail`/`$src`/`$prov` keys,
> `is_fault`, `propagate_fault`) lives in `xi/xi_pack_contract.hpp` — doc 15's
> implementation — and is described in [`pack-plane.md`](./pack-plane.md).

## Provenance (`$src` / `$prov`)

A Record can carry **where it came from**, so non-image data flow is
self-describing without parsing the script:
- the host stamps an instance's output with its name (`$src`),
- extractors **pipe** that onto the typed values they pull out (`pose.src()`),
- constructors **record** which source each input field came from (`$prov`: field
  → src), so the constructor knows the lineage.

Reserved keys (`$src` / `$prov`), like `$na`, harmless to plugins. Complements the
pipeline-graph image-handle edges: the same dataflow the graph can't trace for
scalar/JSON, now explicit — a future graph can read `$prov` to draw those edges.
(The pack plane reuses the same key *names* with its own shapes — `$src` is the
immediate producer, `$prov` a `/`-joined hop chain, stamped by the door glue
before seal; see [`pack-plane.md`](./pack-plane.md).)

## Iconic types — payload on the image channel

Most nominal types live in the JSON channel; **`Region` is the first whose data is
an image** — a binary mask (CV_8U) under the image key `"mask"`, with frame `w`/`h`
mirrored into JSON for cheap metadata. Still "just a name over a Record"; the
difference is purely *which channel* holds the bytes — so the mask rides the
zero-copy ImagePool and drops straight into OpenCV (`xi::to_cv(region)`). Embedding
a Region as a sub-field copies only its JSON — `Record::set(sub)` deep-copies the
JSON root, not the image map, so the mask must be re-attached explicitly.

## See also

- [`../reference/data-types.md`](../reference/data-types.md) — the type vocabulary + Record/Image contract.
- `xi/xi_types.hpp` (+ opt-in `xi_types_cv.hpp`) + each plugin's `io.hpp`.
