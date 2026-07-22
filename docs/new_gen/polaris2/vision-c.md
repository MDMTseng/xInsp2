# xInsp3 — A Framework With Nothing To Hand-Sync

*An independent next-generation design for the xInsp machine-vision framework,
grounded in a close reading of `polaris_master`.*

| | |
|---|---|
| **Date** | 2026-07-02 |
| **Author** | Independent architect C |
| **Basis** | `polaris_master` @ HEAD (worktree xr-09) |
| **Method** | Direct source read of `backend/include/xi`, `backend/src`, `contract/`, `toolbox/`, `sdk`, the client fleet, and the gate tooling. `docs/new_gen/` and `docs/ext_review/` deliberately unread — this is meant to be an unanchored second opinion. |

---

## 0. Preface — where I'm coming from

The current system is, mechanism-for-mechanism, the most carefully engineered
C++ I have read in this class. The lock-free `ImagePool` (`xi_image_pool.hpp`),
the compute→emit seam with its claim-before-compute ordering gate
(`service_inspect.cpp:301`), the deferred-reclamation `WalkGuard`
(`xi_image_pool.hpp:780`), the SEH-to-C++ crash boundary — these are not the
work of people who need architecture advice on the small scale. So my value is
not to re-derive their mechanisms. It is to stand back and ask the one question
a person inside the code cannot ask about their own work: **what is the shape of
the recurring bug, and does the architecture have a name for its cure?**

My answer, argued below, is that nearly every hard-won fix in this tree is the
same fix — *a mirrored fact drifted away from the thing it mirrored, and someone
added machinery to re-sync it.* The framework already discovered the cure once,
locally, in `xi_health.hpp`. The next generation's job is to make that cure the
spine, not a spot treatment.

---

## 1. Findings from the check

I read for bugs, races, and lies between doc and code. Three are real and
concrete. I will not pad beyond them.

### F1 — The contract gate is red on HEAD: four fixtures are unmapped (real)

`contract/validate.py` enforces **strict coverage**: every `*.json` under the
`scan_dirs` must appear in `contract/fixtures-map.json` or the gate fails
(`validate.py:96-104`). The scan dirs are `protocol/fixtures` and
`contract/examples` (`fixtures-map.json:3`).

`protocol/fixtures/` currently contains `get_health.json`,
`health_changed.json`, `health_changed_state.json`, and `hello.json` — none of
which are in the map. (The map's only `hello` entry points at
`contract/examples/hello.json`, `fixtures-map.json:16`, a *different* file.) So
on any machine where `jsonschema` is installed, the `contract_baseline`/
`contract_schema` ctest that the repo advertises as "green is enforced, not
merely claimed" (`validate.py:18-20`) **fails**.

This is not a latent risk — it is the current state of `polaris_master`. It is
also, ironically, *evidence the gate works*: the health/protocol feature
branches merged their wire fixtures without reconciling the discriminator map,
and the strict gate is the thing catching it. Failure scenario: a CI run (or a
developer running `python contract/validate.py`) fails with four `[coverage]`
errors, and anyone who has learned to expect that gate green will start ignoring
it — the classic "broken-windows" erosion of a gate's authority.

### F2 — README §9 lies about the runner; it contradicts the README's own §Features (real)

The Usage-walkthrough section states plainly:

> "It does **not** yet capture the per-frame inspection **verdict** (OK / NG /
> NA) … Per-frame verdict capture (wiring the result callback) is **planned but
> not yet implemented**" — `README.md:288-296`.

The code says otherwise. `runner_main.cpp` wires `xi_script_set_result_callback`
(`runner_main.cpp:12-18, 434-437`), derives a per-frame `class` from the signed
code (`verdict_class_for_code`, `runner_main.cpp:189-195`), and writes a summary
`counts{ok,ng,na,no_verdict,crashed}` tally. The README's *own* Features →
Deployment section (`README.md:436-442`) correctly describes this. So §9 is both
a doc-vs-code lie and an internal self-contradiction. Failure scenario: a
factory-deployment engineer reads §9 (the walkthrough is the natural entry
point), concludes the runner report is verdict-blind, and builds a redundant
external pass/fail scraper that already exists in the report's `counts`.

### F3 — The wire-protocol version is a split-brain: a hardcoded `abi:1` vs the contract's independent `protocol_version` (real)

`send_hello` emits the wire ABI as a **hardcoded literal**: `"abi":1`
(`service_main.cpp:216`). The contract discipline tracks a **separate**
`protocol_version` inside `contract/baseline/protocol-baseline.json`, bumped by
`baseline_gate.py` only when a breaking wire-shape change is detected — and that
gate, by its own comment, deliberately does *not* read or write the backend
literal. There is therefore no mechanical tie between "the number the backend
announces on the wire" and "the number the contract gate guards." A breaking
change to, say, `run-result` can force a `protocol_version` bump in the baseline
while `send_hello` keeps announcing `1`, or vice versa. Two sources of truth for
one fact, unified (per the contract README) only "at the next cutover." Failure
scenario: a client keys compatibility off the handshake `abi`, the backend's
wire shape breaks, the contract baseline records `protocol_version: 2`, but the
handshake still says `1` — the client connects confidently to an incompatible
peer. This is exactly the drift class F-nothing catches, because nothing owns
the relationship.

### Things I checked that are NOT bugs (so the above is not padding)

- **`ImagePool::writable_data` refcount==1 gate** (`xi_image_pool.hpp:224-229`)
  looks racy but isn't, given the stated caller contract (the caller holds its
  own ref, so a concurrent release can only go N→N-1). The reasoning in the
  comment is sound.
- **`share_out` CAS-claimed enroll under parallel fan-out**
  (`xi_record.hpp:207-230`) correctly defends against the double-enroll leak it
  describes. I tried to find a lane ordering that leaks the reserved ref; the
  "+1 reserved for the adopter, released by whoever takes the JSON branch"
  accounting (`xi_use.hpp:558-574`) balances on every path I traced.
- **The SEH translator throwing on `STACK_OVERFLOW`** (`xi_seh.hpp:40`) is the
  one place I expected a latent crash, but the dispatch workers
  `reserve_fault_stack()` before installing it (`service_dispatch.cpp:258`),
  which is the correct guard.

These are genuinely solid. My criticisms below are architectural, not
correctness.

---

## 2. The shape of the recurring bug

Before designing, name the pattern. Read the comments in this tree as a
changelog of pain and one failure mode dominates:

- The instance-state map "kept drifting out of sync" until it was **moved into
  the PluginManager** so create/remove/rename migrate it atomically
  (`service_dispatch.cpp:616-622`).
- The carved `xi.imaging`/`xi.doc`/`xi.emit`/`xi.log` interfaces are
  byte-for-byte copies of the `xi_host_api` struct fields, and a runtime
  **freeze-guard** (`door_matches_fields`, `xi_image_pool.hpp:698-740`) exists
  solely to assert the two copies never drift.
- The `emit_record` door needed a **published-slot bridge**
  (`xi_image_pool.hpp:592-640`) because the carved interface and the struct
  field could otherwise point at different code paths — a "dormant landmine,"
  in the author's own words (`xi_image_pool.hpp:600-602`).
- F3 above: the handshake `abi` and the contract `protocol_version` are two
  representations of one fact with nothing tying them.
- The client fleet has **four independent reimplementations** of the same WS
  envelope demux (VS Code `wsClient.ts` + its own `pendingRsp`, the shared
  `ui-components/src/ws-client.mjs`, and the Python `client.py` — with only the
  two browser JS consumers sharing).

Every one of these is the same bug: **a fact was represented in two places, the
two drifted, and the fix was to add synchronization machinery** (a mutex-owned
migration, a freeze-guard assert, a published slot, a "cutover" promise, a
strict coverage gate). The machinery is well-built. But machinery to keep two
copies in sync is a tax you pay forever, and it is only as good as the next
person remembering it exists.

`xi_health.hpp` is the one place someone stopped paying the tax. Its opening
comment is the thesis of my whole design: it stores *only* the three facts no
other owner holds, and for everything else (instance base state, dispatch
groups, source liveness) it **reads from the existing owner at answer time, so
they cannot drift** (`xi_health.hpp:11-17`). It even names the bug it is
avoiding: "the exact hand-synced-representation failure the v3 architecture
exists to kill." That principle — **one owner per fact; derive on read, never
mirror** — is correct, and it is under-applied. The next generation applies it
everywhere.

---

## 3. The next-generation design

Same philosophy: speed-first, minimal core, functionality-as-plugins, one
machine, C++ scripts, WebSocket, VS Code + HMI. I keep the spine. I diverge on
*how the boundaries are represented* — because that is where the drift tax lives.

### 3.1 Data plane — pooled images: KEEP VERBATIM. The JSON metadata plane: REDESIGN.

**Keep, verbatim, with its mechanism:** the refcounted, generation-stamped,
lock-free `ImagePool`. Handles are `(slot:16 | generation:40)`
(`xi_image_pool.hpp:11-22`); the generation defeats ABA and turns a stale handle
into a clean lookup miss rather than a use-after-free (`lookup`,
`xi_image_pool.hpp:101-111`). Pixels move by handle, addref = cache, pass = zero
copy. This is the heart of "speed-first" and it is correct. I would not touch a
line. The owner-ledger sweep (`release_all_for`, `xi_image_pool.hpp:311-341`)
and the `WalkGuard` deferred reclamation are the right way to make a lock-free
pool safe to introspect. Keep.

**Redesign — this is my boldest divergence: kill the schemaless JSON metadata
plane on the hot path.** The images are the big data and they are already clean.
The *metadata* — thresholds, blob counts, per-blob boxes — is small, and to move
it "zero-serialize" the framework has built an astonishing tower:

- a mutable `yyjson` doc carried across the C ABI by raw pointer
  (`xi_record.hpp` / `xi_abi.h:577-587`),
- a host `DocRegistry` refcount so the doc can be shared across the boundary
  (ABI v4, `xi_abi.h:401-423`),
- copy-on-write freeze semantics with an atomic `frozen_` flag and a `DocBox`
  (`xi_record.hpp:94-125, 752-771`),
- a `yyjson_layout_stamp()` (`xi_record.hpp:77-81`) so a plugin built against a
  different yyjson silently falls back to the JSON path,
- a `share_out`/`adopt_shared` handshake with a CAS-claimed enroll and a
  reserved ref (`xi_record.hpp:207-252`),
- a thread-local pooled chunk allocator (`DocChunkPool`,
  `xi_image_pool.hpp:493-495`) to keep the doc off the malloc hot path.

That is six interlocking subsystems whose entire job is to avoid serializing a
small JSON object. Measure the thing being optimized: a blob record is a few
hundred bytes; a `cv::GaussianBlur` on the same frame is microseconds of SIMD.
The serialize this machinery avoids is *noise* next to the CV op it rides
alongside. Meanwhile the machinery itself is the single largest source of
subtle, refcount-shaped, cross-thread complexity in the codebase (every one of
the `git`-comment horror stories about frozen/shared/registry-managed docs lives
here).

My replacement: **a typed, flat, POD-ish record on the hot path; JSON only at
the wire edge.** Concretely — the cross-ABI record is a small tagged
key→value buffer (int/double/bool/string/image-handle/nested-span), laid out
contiguously, passed by pointer, *copied* on the rare cross-instance hop (a
memcpy of a few hundred bytes is cheaper than one atomic CAS on a contended
DocBox, and it is trivially thread-safe with zero freeze/COW/registry logic).
Images still ride as handles inside it (the one thing that must stay zero-copy).
JSON serialization happens exactly once, at the recorder or the WS boundary,
where it is genuinely I/O-bound and nobody cares. This deletes the DocRegistry,
the doc chunk pool, the COW/freeze state machine, the layout stamp, and the
share_out/adopt handshake — five subsystems — and it makes the record boundary
something a person can hold in their head. I am betting throughput is *identical*
(the images never moved) and the correctness surface shrinks by an order of
magnitude.

### 3.2 The plugin boundary — C ABI: KEEP. The dual-path host table: DELETE one path. The schemaless Record: INVERT the default.

**Keep:** the stable C ABI as the plugin contract. "No C++ types cross the
boundary" is the correct call for surviving MSVC drift, and the `create /
process / destroy / exchange / get_def / set_def` verb set
(`xi_abi.h:704-716`) is the right minimal shape. Keep the
`instance_folder`-for-heavy-assets convention (`xi_abi.h:340-348`) and the
optional `prepare`/`commit` frame-perfect config swap (`xi_abi.h:710-716`) —
that staged-load-then-atomic-swap is a genuinely good primitive.

**Delete — the monolith the "monolith retirement" did not actually retire.**
ABI v10 introduced `get_interface(id, version)`, the CLAP-style capability door
(`xi_abi.h:487-516`), and v11 was billed as "RETIRE THE MONOLITH"
(`xi_abi.h:135-149`). But it retired only the *legacy whole-table view*
(`xi.legacy@9`). The flat `xi_host_api` struct fields **all still exist**
(`xi_abi.h:324-517`), *and* the carved per-capability interfaces exist
(`xi.imaging`, `xi.doc`, `xi.emit`, `xi.log`, `xi.imaging_rw`), and they are
byte-for-byte the same pointers, kept in lockstep by a runtime assert. So the
"retirement" *doubled* the surface: now there are two ways to reach every host
capability, plus a freeze-guard (`door_matches_fields`) and a published-slot
bridge (`publish_emit_record`) whose only reason to exist is that the two copies
must never drift. This is the recurring bug, shipped as an ABI.

My call: **pick one path and delete the other.** Go pure capability-query. The
host table is a single `get_interface(id, version)` pointer and *nothing else* —
every capability (imaging, doc→(now typed record), emit, log, preview) is a
frozen, independently-versioned interface struct behind that door. No flat
fields, so no field-vs-door duplication, so no freeze-guard, no bridge, no
`canonical_host_api()` that must be kept identical to the carved copies. A plugin
resolves the interfaces it needs once at `create` and caches the pointers. This
is *more* minimal than today, not less; it is what "retire the monolith" should
have meant. The cost — old plugins that read struct fields must rebuild — is a
cost v11 already imposed (min-compat raised 6→11, `xi_abi.h:156`); I am simply
saying finish the job instead of carrying both.

**Invert the default — make the typed contract the primitive, schemaless the
escape hatch.** The schemaless Record was chosen for flexibility. But look at
what every *serious* plugin does with it: `blob_analysis` re-adds compile-time
safety with a hand-written typed wrapper (`blob_analysis_io.h`'s `Input`/`Output`
builder/extractor), re-adds runtime safety with a schema stamp and
`xi::contract::check_schema` (`blob_analysis.cpp:132`), and the ABI even carries
an *optional* `xi_plugin_record_schema` export for load-time field validation
(`xi_abi.h:718-731`). That is **three overlapping mechanisms** — compile-time
wrapper, runtime stamp check, load-time declared schema — all bolted on to make a
schemaless container safe again. When every real user of a primitive
reconstructs the same discipline on top of it, the primitive has the wrong
default.

So: the boundary record type is **typed and schema-declared by default**. A
plugin declares its produces/consumes contract *once* (the `_keys.h` +
`_io.h` + schema the good plugins already write by hand), and the framework
*generates* the typed builder/extractor, the wire codec, and the load-time
validator from that one declaration. The schemaless bag survives only as an
explicit `xi::AnyRecord` escape hatch for genuinely dynamic data (a passthrough
logger, a debug dump). This is not a new idea I am imposing — it is the pattern
`toolbox/blob_analysis` *already invented*. I am promoting it from convention to
primitive, and generating the boilerplate the plugin author currently writes
three times by hand.

### 3.3 The script experience — C++ JIT: KEEP, and lean into it harder.

`xi_script_compiler.hpp` is excellent and under-appreciated. The vcvars
environment is captured *once* and cached as a `CreateProcess` block
(`xi_script_compiler.hpp:457-487`) — killing the ~1-2s-per-compile tax; the
OpenCV umbrella is PCH'd (`ensure_pch`, `xi_script_compiler.hpp:517-548`); the
raw-`#pragma omp` guard (`raw_omp_pragma_lines`, `:343-370`) rejects the one
construct that would silently escape SEH crash isolation and leak owner=0 pool
images, and steers the author to `xi::parallel_for`. Diagnostics are transcoded
to UTF-8 for non-English toolchains (`ensure_utf8`, `:138-163`) and
`VAR`-redefinition errors are augmented with the exact duplicate line numbers
(`augment_var_redefinitions`, `:261-316`). This is a *product*, not a build step.
Keep all of it verbatim.

**Where I lean harder:** the JIT is the framework's best differentiator and it
should be the delivery vehicle for §3.2's typed contract. When a script writes
`xi::use("det0")`, the compiler already knows `det0`'s plugin. It should
force-include that plugin's *generated* `_io.h` so `det0.process(...)` is typed
end to end — a key typo is a compile error, the return is a typed extractor,
with zero hand-written wrappers. The JIT makes this free in a way a pre-compiled
SDK never could: the types are regenerated from the live project's plugin set at
compile time. This is the payoff for choosing C++-JIT over a scripting VM, and
the current design leaves it on the table.

One thing I would *not* do: I would not add a graph authoring editor. The README
is right that "the script is the source of truth" (`README.md:100-103`). A
read-only pipeline view is the correct amount of graph.

### 3.4 Protocol & contract — the schema is descriptive; MAKE IT GENERATIVE.

The contract discipline is thoughtful: a constrained JSON-Schema subset (no
`oneOf`/`anyOf` so every message maps to a flat struct), a discriminator that
lives in `fixtures-map.json` because the subset bans unions, a baseline gate that
classifies deltas as unchanged/additive/breaking and *refuses to write a
breaking change without a version bump* (`baseline_gate.py`). The instinct —
freeze the wire shape, force a version move on breaks — is exactly right.

But it is **descriptive, not generative** (the contract README calls itself a
spike; `codegen/gen_types.py` emits types for exactly one payload as a *proof*,
and nothing regenerates or diffs them in a gate). The backend structs, the
handshake `abi` literal (F3), and the client codecs are all *separate* real
sources that the schema *mirrors*. That is the recurring bug again, dressed as a
contract: the schema and the backend are two representations of one wire, synced
by a fixture round-trip whose strength is only as good as its fixture coverage
(and F1 shows that coverage just drifted).

My redesign: **the schema is the single source, and it generates.** From one
schema set, code-gen produces (a) the backend's C++ record views, (b) the
handshake version number (so F3 cannot exist — there is one number, emitted from
the generated header), and (c) the four client codecs (see §3.6). The gate stops
being "does the fixture still match the frozen shape" and becomes "is the
generated code checked in and identical to a fresh generation" — a diff nobody
can forget to run because the build regenerates and fails on a dirty tree. The
baseline gate's version-bump-on-break discipline stays; it just guards a
generative source instead of a descriptive mirror.

### 3.5 State / health / lifecycle — KEEP `xi_health.hpp` VERBATIM and make it the template.

`xi_health.hpp` is the best-designed file in the tree and the model for
everything else. It stores the three facts no one else owns (top state, script
health, per-instance fault overlay) and derives the rest at read time from the
`PluginManager`, the lanes, and the `TriggerBus` — explicitly to prevent drift
(`xi_health.hpp:11-17`). Updates touch an atomic word + a lifecycle-rate map,
never the per-frame path (`xi_health.hpp:19-22`). It renders a tiny FE-mirror
file the supervisor polls (`mirror_json`, `:284-293`) because the FE holds no WS
client. Keep every line, and — this is the design's spine — make "single owner,
derive on read" the rule the whole framework is measured against.

**Keep** the fault-policy vocabulary (`xi_fault_policy.hpp`): `reuse / reinit /
refuse` with escalation-after-N-failures is the right model for post-crash
instance handling. **But I diverge on the default.** Today the default is `Reuse`
(`xi_fault_policy.hpp:18, 40`): a caught access-violation logs the fault and puts
the *same instance* back into service on the next frame. For a stateless operator
that is fine; for a *stateful* plugin caught mid-mutation it means shipping
possibly-corrupt state forward — and after an AV, the honest posture is that you
do not know which you have. I would make `reinit` the default for any plugin that
declares itself stateful (has a non-trivial `get_def`/`set_def`), and reserve
`reuse` for explicitly-stateless operators. The framework already has all the
machinery; it is pointed at the more dangerous default.

### 3.6 The client fleet — ONE generated transport, four thin shells.

Today there are four hand-written implementations of the same envelope demux:
VS Code's `wsClient.ts` plus its *own* `pendingRsp` correlation map, the shared
browser shim `ui-components/src/ws-client.mjs`, and the Python `client.py`.
Only the two browser JS consumers share. The VS Code extension and the shim are
*both JS in the same repo* and still duplicate request/response correlation,
close-code/busy handling, and the `instances`-side-channel workaround — the
clearest consolidation target in the fleet.

This is §3.4's finding on the client side. The cure is the same: **generate the
transport core from the protocol schema.** One code-gen target per runtime (TS,
Python, and the C++ used by the runner/tests), each producing the envelope
codec + id-correlation + busy semantics from the single schema. The VS Code
extension, the HMI, and the Python SDK become thin *policy* shells (spawn/attach
lifecycle, reconnect cadence, UI wiring) over a generated *transport* they all
share by construction. The extension's 3,000-line god-file
(`extension.ts`) sheds its hand-rolled protocol layer entirely.

**Keep** the deliberate decision the whole fleet already shares: the WS layer is
*generic transport only*; all vars/image/preview decoding lives in each plugin's
own webUI. This is why the "Variable Window" is gone from the extension and why
`expose` (`toolbox/expose/plugin.json`) is a *plugin* that JPEG-encodes
subscribed channels and pushes them as one atomic XEX1 binary frame via
`emit_binary`. That is the philosophy working: the core knows nothing about
"vars," a plugin owns the data-out surface, and the client just renders what the
plugin's UI defines. Keep it.

### 3.7 Testing & gates — KEEP the freeze discipline, make the gates un-skippable where they matter.

The two binary tripwires — `test_abi_freeze` (pins every field offset + type of
the frozen table) and `test_golden_plugin` (loads a real plugin through the real
path and asserts a stale plugin is *refused*) — genuinely bite, and the
perf-gate's environment-fingerprint-aware skip (fail only within a matching
hardware class) is a mature touch. Keep them.

**The gap:** several gates *self-skip* when their interpreter or package is
absent — `contract_schema`, `doc_coverage`, `compat_manifest`, the perf gates
all `sys.exit(0)` / SKIP on a bare box (`validate.py:35-39` is the archetype).
CI installs the deps so they bite there, but on a developer's machine they
silently pass. Combined with F1 (the contract gate is currently red), this is how
a gate loses authority: it is red where it runs and green where it does not, so
the signal is ambient noise. My rule for the next generation: **a gate that
guards a checked-in generated artifact cannot skip** — because it needs no
external package, only a diff against the regenerated output. Moving the contract
and compat gates onto generated sources (§3.4) makes them dependency-free and
therefore un-skippable, which is the only kind of gate that keeps its teeth.

### 3.8 Runtime topology — KEEP the FE/BE split and in-process plugins; be honest about the crash trade.

**Keep:** the FE-supervisor / BE-worker split with a single WS client slot; the
headless `xinsp-runner.exe` as the production face; in-process plugins (the
2026-05 removal of process isolation + SHM was the right call for a speed-first,
one-machine framework — the SHM stubs lingering as null fields until v11 was the
only debt, now paid). Keep the per-lane worker pools with the claim-before-
compute ordered-emit gate (`service_dispatch.cpp` + `service_inspect.cpp:301`) —
that is a clean solution to "keep the wire stream in frame order under parallel
dispatch."

**Be honest about the crash trade, and encode the honesty.** In-process SEH
translation (`xi_seh.hpp`) is clever and worth keeping, but catching an
`ACCESS_VIOLATION` and *continuing the same process* is not a correctness
boundary — if the fault corrupted a heap or pool invariant, the "recovered"
backend is in undefined state. The code already half-admits this: a wedged
inspect during teardown does not try to unwind, it `std::_Exit`s for the
supervisor to respawn (`service_dispatch.cpp:448-462`). That instinct is right,
and it should be the *model*, not the exception. The real isolation boundary is
the FE supervisor + respawn; the SEH catch is a graceful-degradation and
diagnostics tool layered on top. So I would: (1) keep SEH-catch for turning a
user null-deref into a clean per-frame error (its best use), (2) make the
post-fault default `reinit` for stateful plugins (§3.5) rather than reuse-after-
AV, and (3) treat repeated faults in a window as a signal to escalate to a
supervised respawn rather than catch-and-continue indefinitely. Same machinery,
honest posture.

---

## 4. The one thing

**Elevate the `xi_health.hpp` principle — *one owner per fact, derived on read,
never mirrored* — from a health-contract tactic to the architecture's central
invariant, and make every cross-boundary representation (the plugin ABI, the
wire protocol, the four clients, the version numbers) *generated from a single
source* so there is nothing left to hand-sync.**

I would bet the next generation on this because the evidence is already in the
tree: nearly every scar in the codebase is a drift wound, and every fix is
synchronization machinery. The instance-state map moved into its owner. The
carved interfaces need a runtime assert to stay equal to the struct fields. The
emit door needs a published slot to reach the same code path as the field. The
handshake `abi` and the contract `protocol_version` are the same number twice
with nothing tying them (F3). The four WS clients are the same demux four times.
The plugins re-declare their schema three ways to make a schemaless container
safe.

`xi_health.hpp` is the counter-example that proves the cure: it refused to
mirror, derived instead, and it is the one subsystem with no drift story to tell.
The current architecture treats that as a clever local choice. The next
generation should treat it as law — and where a fact genuinely must exist in two
languages or two binaries (the ABI, the wire, the clients), it must be *generated
from one declaration*, not written twice and guarded by a test. A framework whose
boundaries are generated from single sources doesn't need freeze-guards,
published-slot bridges, cutover promises, or strict-coverage gates to stay
coherent — it is coherent by construction, and it stays fast because the one
thing that must never be copied, the image pixels, already never are.
