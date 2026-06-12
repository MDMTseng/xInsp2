# Typed I/O wiring + NA propagation

How instances connect to each other ergonomically and robustly, without adding
a schema/type system to the core. Status: **design + phased build** (Phase 1 in
progress).

## The problem

The script is the composition layer (plugins can't call plugins — see
[`../guides/writing-a-script.md`](../guides/writing-a-script.md)). Wiring one
plugin's output into the next plugin's input is hand-written cJSON juggling, and
every connection is a place where data might be missing — so the script fills
with null-checks and error handling.

Worked example (fixturing / pose-aligned inspection):

- A `matcher` instance locates a part → outputs **N orientations** (poses).
- A `line_fit` instance fits a line. Its search line is authored in a **baseline**
  pose frame; at runtime it's transformed by `T = current ∘ baseline⁻¹` to the
  **current** pose, then fit. `baseline_pose` + `line_params` are `line_fit`
  config (taught once); only `current_pose` flows in per frame.

So the per-frame wiring is: pick the right orientation out of the matcher's N,
and feed it as `line_fit`'s `current_pose`.

## The shape of the answer

Three ideas, none of which puts a schema in the core:

### 0. Cost — the wrappers are shallow views

`xi::Typed` is either OWNED (holds its own Record) or a VIEW (shares a parent's
`shared_ptr<Record>` + points a `cJSON*` at a sub-node — no copy). Extractors
hand out views, so pulling N nested values / array items costs no cJSON
duplication; the one materialising copy is `record()`, when you embed a value
into a constructor input. Measured (`examples/io_stress/bench.cpp`, /O2, 100×
extract+construct over a 5-feature output): deep-copy wrappers ran 2.3× a bare-
Record baseline; the view wrappers cut that to ~1.4× — ~3× less wrapper overhead
(≈21 µs/iteration). In real use you `extract(process(...))` (an rvalue moved into
the shared root, no copy), so it's cheaper still. The type NAME is free; the cost
is only the cJSON the wrapping would otherwise duplicate.

**Write-through, with explicit clone.** Views are shallow, so `set()` is
write-through *by design* (like a NumPy view): it mutates the node in the shared
tree, so the change is seen by re-reads and flows into inputs you construct from
the value. The extractor OWNS its source (the `process()` result is moved in), so
"the original" you write to is the extractor's record — which is what flows
downstream. When you DON'T want that, `.clone()` (or `record()`) first for an
independent copy:

```cpp
auto e = synth_io::extract(syn.process(...));   // e owns the result
e.roi().set("w", 999);          // write-through: e's roi.w is now 999
e.roi().clone().set("w", 111);  // independent — e's roi.w stays 999
```

(A separate lvalue you pass to `extract()` is taken by value — a copy — so it
stays independent; `extract(process(...))` directly is the common zero-copy case.)

### 1. Nominal types — names over a generic Record

A small set of **nominal type wrappers** — `Number`, `Point`, `Line`, `Arc`,
`Pose`, `Roi` (in `xi/xi_types.hpp`) — each is *just a name* over a generic
`xi::Record`. No fields are enforced; the payload is still schema-less cJSON. The
wrapper is a lightweight handle (holds a Record), can carry schema-less accessors
(`pose.angle()` reads `rec["angle"]`, NA if absent), and **can be NA**.

The name is the same vocabulary in four places:

- the return type of an **extractor**,
- the parameter type of the next **constructor**,
- the `kind` in the plugin **manifest** (so hover / graph / UI are type-aware),
- and the port type the (future) wiring UI uses to decide what connects to what.

The compiler stops you wiring a `Line` into a `Pose` input — but the data never
left generic cJSON, and the `process()` ABI stays untyped (`Record` only). Types
live purely in the wiring layer.

Plugin/toolbox authors define their own nominal types in their own `io.hpp` (it's
just source) — e.g. a toolbox's `MatchResult`. The core ships only the common
vocabulary.

### 2. Per-plugin `io.hpp` — extractor + constructor helpers

Each plugin ships an `io.hpp` (header-only, alongside `plugin.json`) that the
script `#include`s. It mirrors the manifest:

```cpp
auto e = matcher_io::extract(rec);   // a facade, one getter per output port
e.count();                           // -> Number
e.orientations();                    // -> std::vector<xi::Pose>   (whole array)
e.orientation(i);                    // -> xi::Pose                (i-th, lazy)

auto in = line_fit_io::build()       // a facade, one setter per input port
            .current(e.orientation(k))
            .baseline(/* from config */)
            .build();                // -> xi::Record (complete)
line_fit.process(in);
```

**Extractors and constructors are total — they never fail.** A missing field
yields an empty / NA value, not an exception. The wiring code is a straight line:
no null-checks, no try/catch.

### 3. NA propagation — validation at the compute boundary

Validation happens where the data is *used*: inside the plugin's `process()`.

- If the **whole input Record is NA**, the framework short-circuits:
  `use("x").process(na)` returns NA without running the plugin.
- If **some fields are missing**, the plugin checks and bails to NA in one line:

  ```cpp
  if (auto na = xi::require(in, {"current", "baseline"})) return *na;
  ```

A plugin that can't proceed returns `xi::Record::na("line_fit: missing current")`
— an **empty output carrying the reason**. The next stage's input is then NA, so
it short-circuits too: **NA flows to the end of the pipeline** carrying its
reason, with no defensive code in between.

NA is a first-class `Record` concept, represented as a reserved `"$na"` key so it
crosses the `process()` ABI and the WS unchanged:

```cpp
xi::Record::na("reason")   // { "$na": "reason" }, no images
rec.is_na()                // true
rec.na_reason()            // "reason"
```

This extends the existing run-level NA (`xi::result` code 0 = NA) down to the
Record level so it flows *between* stages.

## Provenance (src id)

A Record can carry **where it came from**, so the non-image data flow is
self-describing without parsing the script:

- the host stamps an instance's output with its name (`$src`),
- extractors **pipe** that onto the typed values they pull out (`Pose::src()`),
- constructors **record** which source each input field came from
  (`$prov`: field → src) — so the constructor literally knows the lineage.

```cpp
auto loc_out = loc.process(...);            // $src = "loc"
auto pose    = blob_io::extract(loc_out).orientation(0);  // pose.src() == "loc"
auto in      = line_fit_io::build().current(pose).build(); // in.prov_of("current") == "loc"
```

This complements the pipeline graph's image-handle edges: it's the same dataflow
the graph couldn't trace for scalar/JSON, now carried explicitly through the
typed wiring — a future graph can read `$prov` to draw those data edges. Reserved
keys (`$src` / `$prov`), like `$na`; harmless to plugins.

## Teach-baseline

`baseline_pose` + `line_params` are `line_fit` **config** (instance.json), not a
per-frame input. Teaching = run `matcher`, pull the pose with the extractor, and
store it into `line_fit`'s config via an `exchange` command. The baseline→current
transform is the plugin's own internal job; the framework only delivers the three
pieces.

## Phases

1. **NA backbone** — `Record::na` / `is_na` / `na_reason`, `process()` full-NA
   short-circuit, `xi::require`. Independently useful: every pipeline gets clean
   failure propagation. *(in progress)*
2. **`xi_types.hpp`** — the nominal type wrappers (lightweight handles) + their
   schema-less accessors. *(done)* `Typed` base + `Number / Point / Pose / Line /
   Arc / Roi`; each a shared-Record handle with `record()`, `is_na()`,
   `na_reason()`, a typed `::na(reason)`, field ctors, and schema-less accessors.
3. **`io.hpp` pattern + a fixturing demo** — `extract()` / `build()` facades, a
   locator → line_fit pose-aligned example, manifest `kind` using the type names.
   *(done)* See [`examples/fixturing_demo`](../../examples/fixturing_demo)
   (extractor adapts raw centroids → `Pose`; `line_fit` validates via `require` →
   NA; NA propagates into the typed `Line`).
4. **Wiring UI (later)** — use the port types to connect output→input on the
   pipeline graph and to teach baselines.
