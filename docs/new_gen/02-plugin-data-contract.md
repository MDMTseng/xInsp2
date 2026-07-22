# xInsp3 — Plugin Data Contract: Schemaless Record + Published Schema

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Status** | Proposal (design decision record for the question: schemaless Record vs. typed structs from plugin headers) |
| **Author** | Claude (extends [`01-xinsp3-architecture.md`](./01-xinsp3-architecture.md) change #1) |
| **Decision** | Keep the schemaless `Record` as the only data currency across the ABI. Do **not** pass plugin-defined structs across the ABI. Add a per-plugin **published schema**, and **generate** typed accessor wrappers from it. |

## The question

xInsp2 moves all script↔plugin data as a schemaless `Record`: a zero-copy
JSON document plus a named-image bag. The alternative for xInsp3 is letting
each plugin ship typed data structures in its header and passing those across
the ABI. Which should v3 use?

## Option A — typed structs across the ABI: rejected

Compile-time safety and autocomplete are real benefits, but the option fails
three structural tests:

### A1. It multiplies the frozen-ABI problem by N

The system currently has exactly **one** layout contract (`xi_host_api`),
defended by ADR-001, two build-failing guards, and load-time negotiation —
the best-engineered boundary in the project (review 06, A−). A struct in a
plugin header that crosses the ABI is a layout contract too — one per plugin,
with none of that machinery.

The killer is hot-reload: the script is JIT-compiled in the field against
whatever headers are present, while the plugin DLL in memory may be older or
newer. With schemaless Records, skew degrades to "unknown key ignored /
missing key defaulted" — detectable, recoverable, honest. With structs, skew
is **silent memory corruption**. Hot-reload turns that from a rare packaging
mistake into a daily hazard. This re-creates, per plugin, exactly the failure
class ADR-001 exists to kill.

### A2. It breaks generic plugins — the proof of the philosophy

`record_save`, `record_load`, `expose`, `data_output` work precisely because
a Record is self-describing: they persist, replay, surface, and forward data
from producers they have never heard of. Typed structs make every generic
intermediary producer-aware — replay would need every plugin's headers to
deserialize what it recorded. "The core (and middleware) holds zero
plugin-specific knowledge" stops being true at the data layer.

### A3. Structs stop at the process boundary

A Record's JSON flows unchanged to the WS protocol, the Python SDK, the HMI,
and disk. A C struct needs a hand-written serializer at each of those
boundaries — which is the manual-synchronization failure mode reviews 06/07/10
documented, reintroduced at the data layer.

The performance argument for structs is weak here: per-frame cost is
dominated by images, which already move by pooled handle; yyjson field access
is cheap; and the generated wrappers below compile to the same Record
operations a careful hand-written call site would make.

## Option B — status quo pure-schemaless: also rejected

Review 11 documented what unconstrained schemaless costs in practice: the two
most-copied plugins hand-parse commands with `cmd.find("\"key\"")` +
`std::stoi`; string-key typos fail silently at runtime; config parameters are
undiscoverable and unvalidated; every plugin README is the only record of its
keys, and READMEs drift. The data layer works, but every author pays a
correctness tax that tooling should be paying.

## Decision — Record stays the currency; schema becomes the contract; typed is a generated view

Three layers, strictly ordered by what owns what:

### 1. ABI layer (unchanged): schemaless Record

Zero-copy JSON + named images remains the only thing that crosses the C ABI
and the wire. Generic plugins, record/replay, expose, clients, persistence
all keep working against one universal shape. Skew stays a data-level,
recoverable condition — never a layout-level one.

### 2. Contract layer (new): every plugin publishes a schema

A plugin ships a machine-readable I/O schema — in its manifest, or served via
a carved `describe()` interface (consistent with the `get_interface`
pattern):

- **inputs**: expected Record keys + image slots, types, ranges, required vs.
  optional
- **outputs**: produced keys + image slots, types
- **config**: instance parameters, types, ranges, defaults
- **schema version**, distinct from the plugin's code version

The schema lives in the plugin's package and is registered in `contract/`
tooling (architecture change #1) — same source-of-truth rule as the protocol.

### 3. View layer (generated): typed wrappers, validation, UI, docs

From the published schema, the build generates:

- **Typed C++ accessor headers** for scripts:
  `blob::Params{.threshold = 128}` / `blob::Result r{det.process(...)}` —
  key typos and type mismatches become compile errors. The wrapper compiles
  down to ordinary Record `set`/`get`; the ABI never sees a struct, so
  wrapper-vs-plugin skew degrades exactly like hand-written Record code, and
  the load-time schema-version check reports it precisely instead.
- **Dev-mode validation**: the host (or the SDK test harness) checks Records
  against the declared schema at the `process()` boundary in debug/dev runs.
  Release hot path pays nothing.
- **Instance config UI**: the extension/HMI render config forms from the
  schema instead of hand-built webviews per plugin.
- **Python typing** for `xinsp_py`, and the keys section of each plugin's
  docs — generated, so review 11's "README shows retired API" class of rot is
  impossible.

### Skew semantics (the part structs can never give)

At project load, the host compares the script's compiled-against schema
versions (embedded by the generated headers) with each loaded plugin's
declared schema version:

- equal → silent
- compatible (minor: added optional keys) → proceed
- incompatible (major) → precise, human-readable refusal — the same UX the
  plugin ABI gate already delivers

At runtime, unknown keys are ignored and missing optional keys default —
schemaless tolerance is retained by design, now with the mismatch *visible*
in the health contract instead of silent.

## Adopted direction — builder/extractor headers (stage 1)

*Recorded 2026-07-02, maintainer decision.* The concrete v3 form of the view
layer: each plugin's header ships an **input data builder** and an **output
data extractor** (hand-written, compiling down to Record `set`/`get`), and the
plugin **fails loudly at analysis time** when a required input key is missing.
The ABI currency stays the schemaless Record, per the decision above. Four
guards keep this hand-written form from re-creating drift:

1. **One key-constants header per plugin.** Key names are defined exactly
   once (`blob_keys.h`-style `inline constexpr` names); the builder, the
   extractor, *and the plugin's internal reader* all compile from it. A key
   rename cannot drift between the header wrapper and the implementation —
   the single-source rule without codegen.
2. **Structured missing-input errors, script-owned verdict.** A missing
   required key produces a structured failure (reason code + missing key +
   expected type) returned to the script — not a log line. The script maps it
   to the verdict (typically NA or a system code), because only the script
   knows whether this plugin is on the critical path. Required-key absence is
   always fail-fast; unknown extra keys are ignored + logged once (schemaless
   tolerance retained). Exceptions still never cross the ABI.
3. **Schema-version stamp for precise skew errors.** The header carries a
   schema version constant; the builder stamps it into the Record; the plugin
   checks compatibility on first `process()`. A header-v3 / plugin-v2 skew
   then reports as exactly that — not as a puzzling "missing key" that looks
   like a script bug.
4. **Mechanical header conventions, so stage 2 is a swap.** Builders and
   extractors follow a fixed naming/layout convention. When the generated
   toolchain (above) arrives, the hand-written headers become generated
   artifacts with zero call-site changes; config-UI forms, Python typing, and
   generated docs light up then. This section is stage 1 of the same road,
   not a fork.

## What this costs

- A codegen step per plugin (same toolchain as architecture change #1 — one
  more input to a build stage that already exists).
- Schema authoring discipline for plugin authors — mitigated by the scaffold
  generating the schema skeleton, and by the SDK test harness failing a
  plugin whose behavior doesn't match its declaration (the schema is *tested*
  truth, not documentation).
- Plugins that are intrinsically generic (`record_save`, `expose`) declare
  open schemas ("any record") — the escape hatch is explicit, not implied.

## Final judgment

Typed-structs-across-the-ABI optimizes the pleasant path (autocomplete,
compile errors) by re-creating the project's worst historical failure class
(unguarded layout contracts) N times over, and it structurally breaks the
generic-plugin composition that justifies the whole architecture. Pure
schemaless keeps the composition but taxes every author with stringly-typed
runtime errors. The resolution is the same one the v3 architecture applies
everywhere: **the shared representation stays universal and dumb; the
contract is declared once, machine-readable; everything ergonomic — typed
headers, validation, forms, docs — is generated from it.** Authors get
compile-time safety; the ABI keeps exactly one frozen layout; skew stays a
reported condition instead of a corruption.
