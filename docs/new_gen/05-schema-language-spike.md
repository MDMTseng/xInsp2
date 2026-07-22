# Schema Language Spike — `contract/` (adoption item 7 / change #1)

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Status** | Spike + recommendation. Informs the [`01-xinsp3-architecture.md`](./01-xinsp3-architecture.md) decision-checklist item *"Schema language for `contract/`"* and unblocks [`03-adoption-map.md`](./03-adoption-map.md) item 7 (and, downstream, item 8 client-core). |
| **Author** | Claude (Polaris `polaris/contract-schema-spike`) |
| **Deliverable** | A working `contract/` package: a constrained-subset meta-schema, descriptive schemas for the current wire, a both-ways validation gate wired as a ctest, a TS+Py codegen probe, a C++ target sketch — and this writeup. Zero backend/client behaviour change. |

## The question

Change #1 makes a top-level `contract/` the machine-readable source of truth for
every cross-boundary shape. The maintainer's open decision is the **language**:
JSON Schema, a typed IDL, or hand-rolled — with one hard criterion from `01`:
*must generate readable C++ that respects the zero-copy pools, not just idiomatic
TS/Py.* This spike answers it by actually building the JSON-Schema option against
the real wire and seeing where it holds and where it fights.

## What was built (all on `polaris/contract-schema-spike`)

- **A constrained JSON-Schema subset**, defined twice: in prose
  ([`contract/README.md`](../../contract/README.md)) and **mechanically** as a
  meta-schema (`contract/meta/xi-contract-subset.schema.json`) that every contract
  schema must itself validate against. The subset is object/array/scalar types +
  `required`/`enum`/`const`/`additionalProperties`/`$ref`/`$defs` + scalar
  range/length. It **bans** `anyOf`/`oneOf`/`allOf`/`not`, `if`/`then`/`else`,
  `dependentSchemas`, and `patternProperties`/`propertyNames`/`unevaluated*`.
- **13 descriptive schemas** for the current wire (`contract/schemas/`): the
  `cmd` and `rsp` envelopes, `hello`, `dispatch_stats`, `run_result` +
  the `xi.run-outcome/1` payload (as its own schema, cross-`$ref`'d), `run_finished`,
  `state_dropped`, per-command `rsp` shapes for `commit_group` / `load_project` /
  `metrics`, the deprecated `vars` orphan, and a `XEX1` binary-frame descriptor sketch.
- **Validation both ways** (`contract/validate.py`), wired as the `contract_schema`
  **ctest**: (A) every schema validates against the meta-schema (subset conformance)
  and is a valid draft-2020-12 schema; (B) every fixture under `protocol/fixtures/`
  **and** `contract/examples/` validates against its mapped schema, and an unmapped
  fixture is a hard failure (strict coverage). Verified it actually **fails** on a
  banned keyword, on a fixture/schema mismatch, and on an unmapped fixture — not a
  gate that can only pass.
- **A codegen probe** (`contract/codegen/gen_types.py`): generates a TS `interface`
  and a Py `TypedDict` from the run-outcome schema. The TS **compiles under
  `tsc --strict`** (with a `@ts-expect-error` proving the `schema` const
  discriminates) and the Py **imports** with the right required/optional split.
  Generated artifacts are committed under `contract/codegen/generated/`.
- **A C++ target sketch** (`contract/codegen/cpp-sketch/run_outcome_view.hpp.sketch`)
  — hand-written, not a generator — showing the yyjson **view** a C++ generator
  should emit: borrowed `yyjson_val*`, `const char*`/`int64` accessors, zero copy.

## Finding 1 — the constrained subset is workable, and worth the constraint

Every one of the 13 schemas fits the subset with **no** banned keyword, and each
is a flat "object with typed fields." That flatness is the whole point: it maps
1:1 onto a TS interface, a Py TypedDict, and a C++ accessor view. The ~130-line
hand-rolled generator handles the entire subset; there was no shape it couldn't
express as a named field. **Recommendation-relevant:** the subset is not a
crippled JSON Schema, it is the *codegen-friendly* core of it. The banned
keywords are exactly the ones with no clean struct/accessor mapping (an `anyOf`
is a runtime-tagged union; `if/then/else` has no field to bind to). Banning them
is what lets the C++ criterion in `01` be met at all.

## Finding 2 — where JSON Schema fought back

Three places, each a real limitation the maintainer should price in:

### 2a. Discriminated unions — the subset can't spell the envelope

The wire is a union keyed on `type` (+ `name`/command). The idiomatic JSON-Schema
spelling is a top-level `oneOf` over the variants — a **banned** keyword. Resolution:
the discriminator lives in the **harness** (`contract/fixtures-map.json` routes each
frame to its schema), not in a schema keyword. This is fine — arguably better, since
each message schema stays independently generatable and the dispatch table can be
*generated* from the per-message `type`/`name` consts — but it means the schema
language alone does **not** capture "which shape applies when." That routing is a
first-class artifact the build must own, not an emergent property of the schemas.

### 2b. Binary frames — JSON Schema is the wrong tool for `XEX1`

The `XEX1` expose frame is `"XEX1"` magic + a **msgpack** body pushed as a binary
WS frame. JSON Schema describes JSON documents, so it cannot describe (a) the
magic-prefix framing, (b) the msgpack type tags (the `fixmap`-only cap that review
10 finding 7 flagged, `bin8/16/32`, etc.), or (c) the raw JPEG bytes. The best the
subset can do is describe the *decoded logical body* with the JPEG payload modelled
as an opaque placeholder string (`contract/schemas/xex1-frame.schema.json`) — and
the two invariants that actually bite (`fixmap` ≤ 15 keys; three hand-rolled codecs
staying in type-agreement) live **entirely outside** any JSON Schema. **This is a
dead end, and the spike says so plainly:** do not try to bring `XEX1` under the same
schema language. Keep it out of scope and cover it exactly as review 10 recommends —
a captured binary fixture + an encoder→decoder round-trip test — or, if the binary
surface grows, a purpose-built binary IDL. `contract/` should own the *text* wire.

### 2c. int64 ids — the subset has no integer width

JSON Schema `integer` carries no width, and codegen to TS `number` (an f64)
silently loses precision above 2^53. The wire already dodges this for the big ids
by carrying them as **strings** (`trigger_id`, `inspection_id`), and the schemas
document that with `format: hex`/`uuid` and prose — but the schema **cannot
enforce** "this integer must stay < 2^53"; `maximum: 9007199254740991` is only
advisory. The live exposure is `run_id` / `$seq`, typed `integer` today. The
recommendation (below) turns this into a rule rather than a hazard.

## Finding 3 — drift the validation surfaced in the existing fixtures

Writing schemas *to the fixtures* (descriptive spike: the fixture is reality, fix
the schema) surfaced three disagreements between the fixtures and
`docs/reference/ws-protocol.md`. These are reported, not silently reconciled:

1. **`commit_group_partial.json` sets `ok: true` on a partial commit.** The
   protocol contract (`ws-protocol.md`, commit_group staged-breaking note) says
   *"`ok: false` on partial is unchanged."* The fixture pins `ok: true`. This is a
   fixture bug — and a sharp example of a subset limit: the rule "status == partial
   ⇒ ok == false" needs a conditional (`if/then` or a `oneOf`), which the subset
   bans, so the gate **cannot** catch it. The `ok`/`status` correlation must be a
   consumer/test assertion, not a schema constraint.
2. **`commit_group_*` fixtures use `{status, committed, canonical, warnings}`** —
   the doc documents `data: { results: [...] }`. The fixture and the reference doc
   describe different shapes; one is stale. Schema follows the fixture; the drift is
   flagged for the maintainer to reconcile.
3. **`metrics_snapshot.json` models `buckets` as a fixed-key object**
   (`le_1..gt_50`, 7 keys) and omits `frames_total`/`frames_ok`/`frames_error`; the
   doc shows `buckets` as an **array** of `{le, count}` over 13 edges plus the
   `frames_*` counters. Another doc/fixture divergence.

Plus the already-known orphan: **`vars_mixed.json`** models the removed `vars`
message (review 10 finding 9; adoption item 4 flags it for retirement). The gate
routes it to a `deprecated` schema and **reports** it as an orphan rather than
deleting it (out of spike scope). All four are exactly the "manual synchronization
drifted" class change #1 exists to kill — and the gate now makes the text-wire
half of that class mechanically visible.

## Tooling — off-the-shelf validation, hand-rolled codegen

| Concern | Tool | Verdict |
|---|---|---|
| Fixture/subset validation | `jsonschema` 4.23 + `referencing` (Python; already a repo QA dep) | Off-the-shelf. Draft 2020-12, cross-file `$ref` via a `Registry`. No reason to hand-roll. |
| Subset enforcement | A hand-written meta-schema, checked by the same validator | ~90 lines; mechanical, no custom code. |
| TS + Py codegen | Hand-rolled ~130-line generator | Deliberate — wanted to measure the generator cost for the subset. It is small precisely *because* the subset is flat. Off-the-shelf `json-schema-to-typescript` / `datamodel-code-generator` would also work for TS/Py but neither emits the zero-copy C++ view, which is the binding constraint. |
| TS compile probe | `tsc` 5.4.5 `--strict --noEmit` | Generated interface compiles; discriminator const enforced. |
| C++ | No generator built (per scope) — hand-sketch only | The sketch shows a generator emitting borrowed-`yyjson_val*` accessors is straightforward from the subset; the flatness is what makes it so. |

The split is the headline tooling result: **validation is a solved, dependency-only
problem; codegen is a small hand-rolled step whose size is bounded by the subset.**
That matches change #1's accepted cost ("a codegen step in the build") without a
heavyweight schema-compiler dependency.

## Recommendation

**Adopt the constrained JSON-Schema subset (draft 2020-12) as the `contract/`
language for the text wire.** It is workable against the real protocol, the schemas
stay flat enough to generate clean compiling TS/Py and a zero-copy C++ view, and the
subset bans exactly the keywords that would break the C++ criterion — validated by
off-the-shelf `jsonschema` plus a ~130-line generator, both already proven green in
this branch. Three riders the maintainer should bless with it:

1. **Keep `XEX1` (and any future binary frame) OUT of the JSON-Schema contract**
   (Finding 2b) — cover it with a captured-fixture round-trip test per review 10, or
   a separate binary IDL; do not stretch JSON Schema over msgpack.
2. **Make "big ids are strings" a contract rule** (Finding 2c): any id that can
   exceed 2^53 is a `string` on the wire (as `trigger_id`/`inspection_id` already
   are); `integer` is only for values provably < 2^53. The generator can flag a
   raw `integer` id field as a lint.
3. **Own the discriminator and the unknown-key policy as build/harness concerns**
   (Findings 1/2a): generate the `type`/`name` → schema dispatch table from the
   message schemas, and keep `additionalProperties: true` on payloads to honour the
   additive-only wire (closed validation is a consumer/test job, since the subset
   can't express the conditional rules — Finding 3.1).

Alternatives considered and rejected: a **typed IDL** (protobuf/Cap'n Proto/TypeSpec)
buys int64 and binary framing but re-imposes a code-generated layout contract on the
wire — the very thing `02` argues against for the plugin data path — and its C++
output is message-copying, not a pooled view; **hand-rolled** schemas are where the
project is today and are precisely what drifted. JSON Schema (constrained) is the
option that generates the pooled C++ view *and* has zero-cost validation tooling.

## Honest dead ends

- **One-schema envelope via `oneOf`** — abandoned; the subset bans it, and the
  harness discriminator is the better factoring anyway (Finding 2a).
- **`XEX1` under JSON Schema** — abandoned as a category error; documented as such
  rather than faked with a lossy string model pretending to be the frame (Finding 2b).
- **Closed (`additionalProperties: false`) descriptive schemas** — abandoned: they
  fight the additive-only protocol and would break on the next added field. The
  `ok`/`status` correlation bug (Finding 3.1) shows the flip side — the subset also
  can't *add* the cross-field rules that closed validation would want. Both are
  consumer/test responsibilities, on the record.

## Three-sentence summary (for the report)

The constrained JSON-Schema subset is workable for the **text** wire: all 13
descriptive schemas fit it with no banned keyword, off-the-shelf `jsonschema`
validates fixtures both ways as a real (failing-when-it-should) ctest gate, and a
~130-line hand-rolled generator produces TS that compiles under `tsc --strict`, a Py
`TypedDict` that imports, and a sketched zero-copy C++ yyjson view — so I recommend
**adopting it**, with binary frames (`XEX1`) kept out of scope and big ids carried as
strings. JSON Schema fought back in exactly three places — discriminated unions (no
`oneOf`, so routing lives in the harness), binary/msgpack framing (a category error
for JSON Schema — cover it with a round-trip fixture instead), and int64 width (the
subset can't enforce it) — none of which block adoption. Validation surfaced real
drift to fix independently: `commit_group_partial.json` pins `ok: true` on a partial
commit (contradicts the doc, and the subset can't catch it), the `commit_group` and
`metrics` fixtures disagree with `ws-protocol.md` on shape, and `vars_mixed.json` is
a confirmed orphan.
