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
  validate.py                           the gate: validates BOTH ways (subset + fixtures)
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
