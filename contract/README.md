# `contract/` — wire-schema spike (contract-first, descriptive)

Status: **SPIKE** for adoption-map item 7 / architecture change #1. This package
describes the **current** xInsp2 wire as-is (non-breaking, descriptive) in a
constrained JSON-Schema subset, validates every fixture against it as a build
gate, and probes codegen. It changes **no** backend or client behaviour. The
maintainer decision it informs — *"schema language for `contract/`"* — is written
up in [`../docs/new_gen/05-schema-language-spike.md`](../docs/new_gen/05-schema-language-spike.md).

## Layout

```
contract/
  meta/xi-contract-subset.schema.json   the constrained-subset meta-schema (enforces the subset)
  schemas/*.schema.json                 descriptive schemas for the current wire
  examples/*.json                       sample frames for schemas with no protocol/fixtures/ fixture
  fixtures-map.json                     fixture -> schema routing (the discriminator; see below)
  live-wire-map.json                    rsp command -> schema routing for the live-wire gate
  live-allowlist.json                   the ratchet: live messages no schema describes (yet)
  validate.py                           the gate: validates BOTH ways (subset + fixtures)
  live_conformance.py                   the THIRD leg: validates the LIVE backend's bytes
  codegen/gen_types.py                  codegen probe (TS interface + Py TypedDict)
  codegen/generated/                    committed generated artifacts + a tsc type-probe
  codegen/cpp-sketch/                   hand-sketch of the C++ yyjson-view target (not generated)
```

## The constrained subset (explicit)

A schema under `schemas/` MAY use only:

- **types**: `object`, `array`, `string`, `integer`, `number`, `boolean`,
  `null`; and `type` as an array of these for nullable (e.g. `["string","null"]`).
- **object**: `properties`, `required`, `additionalProperties` (boolean or a
  subschema).
- **array**: `items`, `minItems`, `maxItems`.
- **scalar constraints**: `enum`, `const`, `minimum`, `maximum`,
  `exclusiveMinimum`, `exclusiveMaximum`, `multipleOf`, `minLength`, `maxLength`,
  `pattern` (on a string *value*).
- **references**: `$ref` to a local `$defs` or to another schema **within this
  package** (by `$id`); `$defs` for local reuse.
- **annotations**: `$id`, `$schema`, `title`, `description`, `$comment`,
  `deprecated`, `examples`, `default`, `format` (documentation only — not asserted).

A schema MUST NOT use (banned — mechanically rejected by the meta-schema):

- the combinators `anyOf` / `oneOf` / `allOf` / `not`,
- the conditionals `if` / `then` / `else`, `dependentSchemas` /
  `dependentRequired`,
- key-pattern / dynamic keywords `patternProperties`, `propertyNames`,
  `unevaluatedProperties` / `unevaluatedItems`.

**Why the exclusions.** The combinators and conditionals are what make JSON
Schema hard to generate readable, zero-copy C++ from — an `anyOf` becomes a
tagged union the generator must discriminate at runtime, and `if/then/else`
has no clean struct/accessor mapping. Banning them keeps every schema a flat
"object with typed fields" that maps 1:1 to a TS interface, a Py TypedDict, and
a C++ view. `patternProperties` is banned because an open key-pattern map can't
be a named accessor; an open map is still expressible where genuinely needed via
`additionalProperties: <subschema>` (a *typed* open map), which the subset keeps.

### The discriminator lives in the harness, not a keyword

The wire is a discriminated union keyed on `type` (+ `name` for events, + command
for responses). The natural JSON-Schema spelling is a top-level `oneOf` over the
message variants — **which the subset bans**. So the routing (which schema a given
frame must satisfy) lives in [`fixtures-map.json`](./fixtures-map.json), read by
the validator. This is a deliberate trade recorded in the writeup: the schemas
stay flat and codegen-friendly; the union lives in one small data file instead of
a schema keyword. A real build would generate that dispatch table from the set of
message schemas (each carries its `type`/`name` consts).

### Unknown-key policy: `additionalProperties: true`

The WS protocol evolves **additive-only** (new fields, old clients ignore them).
A descriptive schema of that wire therefore sets `additionalProperties: true` on
message payloads: a newer backend that adds an identity field must still validate
against today's schema. The cost is that these schemas do **not** catch a typo'd
extra key — closed validation (`false`) would, but it would also break the moment
the protocol adds a field, which contradicts the additive-only rule. Closed
(`false`) is reserved for genuinely closed sub-objects. This tension is a finding,
not an accident — see the writeup.

## Running the gate

```
python contract/validate.py         # both ways: subset conformance + fixture conformance
python contract/codegen/gen_types.py # regenerate the TS + Py artifacts
```

The gate is wired as the `contract_schema` ctest (`backend/CMakeLists.txt`), so
`ctest -C Release -R contract_schema` runs it in CI alongside the other gates. It
needs the `jsonschema` Python package; like the `doc_coverage` gate it self-skips
(exit 0) if the interpreter or package is absent, so a minimal box still builds.

## The third leg — live-wire conformance (`live_conformance.py`)

`contract_schema` proves the **fixtures** match the **schemas**. But both are
hand-authored mirrors of the C++, and nothing checked either against what the
**live backend actually sends**. So a C++ change that alters the wire and skips
the schema+fixture update passes every gate green — the two mirrors still agree
with *each other*; they just no longer describe reality. That is the open loop
this leg closes:

```
   schemas  <——contract_schema——>  fixtures        (the two hand-authored mirrors)
      \                               /
       \————————contract_live————————/
                     |
              the LIVE backend's bytes                (reality)
```

`live_conformance.py` spawns the real `xinsp-backend.exe` on an ephemeral port,
drives a representative session (hello, `dispatch_stats`, `get_health`, open /
compile / run a tiny project, `start`/`stop` to force a `health_changed`
transition, `commit_group`, `load_project`, `metrics`), captures **every** inbound
message, and does three things:

1. **Validates** each captured message a schema describes, against that schema
   with `jsonschema` — the same discriminator logic `fixtures-map.json` encodes,
   now applied to live bytes. **Events** route by their schema's own `type`/`name`
   consts (auto-derived — a new event schema is picked up with no edit). **Rsps**
   carry no `name` on the wire, only an echoed correlation `id`, so they route by
   their **originating command** via [`live-wire-map.json`](./live-wire-map.json)
   (`validate: envelope` vs `data` says whether the schema describes the whole rsp
   or just its `data` payload). Every rsp is *additionally* checked against the
   generic [`rsp.schema.json`](./schemas/rsp.schema.json) envelope — the only place
   that schema is exercised against real bytes. A live message that violates its
   schema **fails** the gate, printing the offender.

2. **Ratchets** every message no schema describes against
   [`live-allowlist.json`](./live-allowlist.json): a listed key is counted and
   reported; an **unlisted** one **fails** ("a new unschema'd wire message shipped
   — add a schema or allowlist it"). A new message type can never reach the wire
   silently undescribed. The allowlist is seeded with what a real session produces
   today (the `compile_started` / `run_started` progress notifications, the `log`
   and `instances` channels, and the rsps whose per-command `data` shape isn't
   modelled yet), each with a reason.

3. **Coverage**: asserts the session actually observed the load-bearing schema'd
   messages, so a green means the wire was exercised, not that the session quietly
   produced nothing (a green with a hole is worse than a red).

**When the live bytes disagree with a schema, the BACKEND is truth.** Fix the
schema (and refresh the baseline — see below — in the same commit); do **not**
"fix" the backend to match a stale schema. That divergence surfacing is this gate
earning its keep.

Wired as the `contract_live` ctest (label `contract`), so it runs in `gate.py`'s
ctest stage against a freshly built backend. Like the other contract gates it
self-skips (exit 0, loud `SKIP - NOT A PASS`) when the backend exe is not built or
a Python dep (`jsonschema`, or the `xinsp2` websocket client) is missing:

```
python contract/live_conformance.py          # needs a built backend/build/Release/xinsp-backend.exe
```

## The protocol-version gate — `baseline/` (`baseline_gate.py`)

`contract_schema` proves each schema is *well-formed* and that fixtures match. It
does **not** notice when a schema **changes**. That is the second gate's job:
`baseline_gate.py` makes the WS protocol version **real** without touching the wire.

### The problem it closes

The `hello` event carries an `abi` stamp — the WS-protocol version — hardcoded to
`1` (`send_hello`, `backend/src/service_cmd_lifecycle.cpp`). That `1` has survived
genuine wire breaks (the removed `vars` message, partial-status returns) without
ever moving, and nothing enforced *when* it should (`docs/ext_review/06` finding 2).
So the **described** shape of the wire could drift silently underneath a stamp that
never changed. The extension now treats `abi != EXPECTED_WS_ABI` as incompatible
(`vscode-extension/src/versionCompat.ts`), so the day the backend bumps the stamp,
clients react — but that only matters if *something decides the stamp must bump*.
This gate is that decision, mechanised.

### The committed baseline

`baseline/protocol-baseline.json` is a **single, reviewable** generated file holding:

- `protocol_version` — the version this snapshot corresponds to (seeded to the
  current live `abi`, **1**). Bumped only on a breaking change (see below).
- `schemas` — for every schema under `schemas/`, its `sha256` **and** its canonical
  `shape` (the shape-bearing keywords, with documentation annotations stripped —
  see next paragraph). The `shape` is what makes a change *classifiable*, not just
  detectable; the `sha256` is the quick unchanged-check and the integrity digest.
- `open_enums` — **hand-curated** policy: JSON-pointer paths whose enum a consumer
  tolerates unknown values on (seeded with `run-outcome.schema.json` `/properties/class`,
  which the schema's own description documents as open). Adding a value to a listed
  enum is *additive*; to any other enum it is *breaking*. This block is preserved
  across refreshes — the refresh only regenerates `schemas` and `protocol_version`.

A digest-only manifest was rejected: it can tell you *that* a schema changed but not
*how*, so it could not distinguish additive from breaking. Full copies of the schema
tree were rejected as duplication. Storing the **canonical shape** is the middle
ground — enough structure to classify, small because annotations are stripped, and
the git diff of this one file shows a reviewer exactly what wire shape moved.

**Canonical shape / what the gate ignores.** Before hashing and diffing, each schema
is reduced to its shape-bearing keywords; the documentation-only annotations the
subset marks "not asserted" (`title`, `description`, `$comment`, `examples`,
`default`, `deprecated`, `format`) are stripped, and order-insensitive lists
(`required`, `enum`) are sorted. Consequence: **editing a description does not trip
this gate** (it is not a wire change); changing a type, property, enum, or constraint
does.

### The additive-vs-breaking ruleset (exact, conservative)

Comparing the baseline shape to the live shape, per schema — when a case is
ambiguous it is classified **breaking**:

**BREAKING** (a consumer built against the baseline cannot absorb it):

- a property is **removed** (or renamed — a remove + add);
- a property's **type changes** (including nullability, e.g. `"string"` →
  `["string","null"]`);
- a property is **added as `required`**, or an existing property is **made required**;
- an existing **required property is made optional** (the presence guarantee is gone);
- an **enum value is removed** (a value producers may still emit becomes invalid);
- an **enum value is added to a *closed* enum** (one not listed in `open_enums`);
- a **`const` changes**, any **scalar/array constraint** changes (`minimum`,
  `maximum`, `minLength`, `pattern`, `minItems`, … — tightening rejects
  previously-valid values, and even loosening can break a consumer sized to the old
  bound, so *any* change is breaking), or a **`$ref` target** changes;
- **`additionalProperties` is tightened** `true` → `false` (unknown fields that used
  to be tolerated are now rejected);
- a **new optional property is added to a *closed* object** (`additionalProperties:false`
  in the baseline — old consumers reject unknown keys);
- a **schema file is removed** (a message shape retired).

**ADDITIVE** (tolerated by the additive-only wire):

- a **new optional property** on an object that tolerates unknowns
  (`additionalProperties` `true`/absent in the **baseline**);
- an **enum value added to an *open* enum** (path listed in `open_enums`);
- **`additionalProperties` loosened** `false` → `true`;
- a **new schema file** (a new message type — the discriminator ignores an unknown
  `type`).

**UNCHANGED** → green. Anything else (e.g. a bare `$id` edit that changes the digest
but no shape-bearing keyword) is treated as additive: non-breaking, but the baseline
must still be refreshed so it stays in lockstep.

### Verdicts and the refresh workflow

The gate (`python contract/baseline_gate.py`, read-only) fails on **any** difference —
the baseline must match the live shape, so a schema cannot change without the
baseline changing in the same commit:

- **unchanged** → green.
- **additive** → **fail**, message: refresh the baseline (no version bump) with
  `python contract/baseline_gate.py --update-baseline`.
- **breaking** → **fail**, message: refresh the baseline **and** bump the version with
  `python contract/baseline_gate.py --update-baseline --protocol-version <N>`, plus a
  pointer to the cutover policy. The refresh command **refuses** to write a breaking
  change unless given a `--protocol-version` strictly greater than the current one;
  for an additive change it writes and keeps the version (a supplied bump is ignored
  with a note). That is what forces a breaking wire change to record a version move.

### Relationship to the `hello` `abi` stamp

`protocol_version` in the baseline and the `abi` integer `send_hello` sends are **the
same version, split in two halves today**:

- This gate owns the **description** half — it decides *when* the version must move
  by refusing to let the described shape drift without it.
- The backend owns the **wire** half — the literal `abi` value. This gate deliberately
  does **not** read or change it: bumping the live `abi` is a **breaking wire change**,
  which rides the coordinated cutover train, not master (`docs/ext_review/00-triage.md`
  "Two branch sets"). The extension already enforces the wire half (`abi != EXPECTED_WS_ABI`
  ⇒ incompatible, `versionCompat.ts`).

They should be **brought together at the next cutover**: when a breaking change bumps
`protocol_version` here, the same coordinated step bumps the `abi` literal in
`send_hello` and `EXPECTED_WS_ABI` in the extension in lockstep, so the number a client
gates on and the number this gate tracks are one and the same. Until then, this gate is
the tripwire that guarantees the stamp *can* no longer silently fall behind the wire.

### Running it

```
python contract/baseline_gate.py                                    # the gate (read-only)
python contract/baseline_gate.py --update-baseline                  # refresh (additive/unchanged)
python contract/baseline_gate.py --update-baseline --protocol-version 2   # refresh + bump (breaking)
```

Wired as the `contract_baseline` ctest (`backend/CMakeLists.txt`):
`ctest -C Release -R contract_baseline`. Pure stdlib (`json` + `hashlib`, **no**
`jsonschema`), so unlike `contract_schema` it never skips for a missing package — it
always runs when a Python interpreter is present.
