# xInsp3 — One Owner Per Fact: A Next-Generation Machine-Vision Framework

| | |
|---|---|
| **Date** | 2026-07-02 |
| **Author** | Independent architect B |
| **Basis** | `polaris_master` @ HEAD (worktree xr-09) |
| **Scope** | A from-scratch design carrying the xInsp philosophy (speed-first, minimal core, functionality-as-plugins), informed by reading the current code — not anchored to it. |

---

## 0. How to read this

I read the code before I read anyone's plans (I deliberately did not open `docs/new_gen/` or `docs/ext_review/`). What follows is my own judgment. Section 1 is a **check pass** — real defects, doc-vs-code lies, and gates that do not bite, each with `file:line` and a failure scenario. Sections 2–11 are the **design**: opinionated, committed, and where I diverge from what the current code chose I say so and argue it. Section 12 is the single bet.

My headline conclusion up front: **the current codebase is genuinely excellent engineering, and its remaining risk is almost entirely one shape — the same fact represented twice and kept in sync by discipline.** The next generation should make that shape structurally impossible.

---

## 1. Findings from the check

The core is hardened to a degree I rarely see. The lock-free `ImagePool` (generation/ABA defense, the `WalkGuard` deferred-reclamation StoreLoad handshake, the drop-exactly-one-ref owner sweep) and the compute/emit split with the `EmitTurn` ordered gate are correct under the concurrency they claim. I did not find a crash-class bug in the hot path. What I did find:

### 1.1 Doc-vs-code lie: the README says the runner has no verdict capture; the code and the README's own Features section say it does

`README.md:288-296` (Usage step 9) states the headless report "does **not** yet capture the per-frame inspection **verdict** … Per-frame verdict capture … is **planned but not yet implemented**." This is false as of this HEAD. `runner_main.cpp:438-440` wires `set_result_callback(result_cb)`, `runner_main.cpp:528-534` derives and tallies the per-frame class, and `runner_main.cpp:543-549` writes `counts{ok,ng,na,no_verdict,crashed}`. The README's own Features section (`README.md:435-442`) correctly describes the implemented behavior. So the document contradicts itself and the code. **Failure scenario:** an integrator reads step 9, believes exit-0 is the only signal available, and builds a pass/fail gate on the exit code alone — silently ignoring the NG tally that is right there in the artifact. This is exactly the class of stale-hedge the project's own doc-coverage gate is meant to catch, and it slipped because the hedge is prose, not a table entry.

### 1.2 The contract schema gate does not bite when its dependency is missing — and it is missing in CI

`contract/validate.py:35-39`: on `ImportError` for `jsonschema`/`referencing` the gate prints `SKIP` and `sys.exit(0)`. The docstring three lines up (`validate.py:19-20`) claims "Wired as a ctest … so green is enforced, not merely claimed." The corroborating investigation of the CI workflow found `jsonschema` is provisioned nowhere (`ci.yml` installs only `pytest`), so in CI this gate **always takes the green skip path without validating a single schema or fixture** — including the strict "unmapped fixture = FAIL" rule (`validate.py:100-104`) that is the whole point of Way-B. **Failure scenario:** a schema is edited to reach for a banned `oneOf`, or a new wire fixture lands with no schema mapping; the gate that exists to stop exactly this reports green, and the drift ships. The gate is real on a developer box that happens to have the package, and a no-op on the box that gates merges. A gate that silent-skips on a missing dependency is worse than no gate, because it advertises a safety that isn't there.

### 1.3 Unsynchronized `fps_` in `synced_stereo` (a shipped example)

`toolbox/synced_stereo/synced_stereo.cpp:68` declares `int fps_ = 10;` as a plain int. It is written on the control thread by `exchange()` (`:45`, the `set_fps` command) and by `set_def()` (`:61`), and read on the worker thread by `run_loop_()` (`:111`). This is a formal data race. `mock_camera` made the identical field `std::atomic<int>` (per the plugin investigation), so the correct pattern is established in-tree and this example diverges from it. **Failure scenario:** benign in practice (a torn read of a small int only mis-times one sleep), but it is a *shipped exemplar* — an author scaffolding a source plugin copies the race. The severity is pedagogical, not operational, which for a reference plugin is still a real defect.

### 1.4 Every-crash slow leak of the input metadata doc on the `xi::use()` path

`xi_use.hpp:558-579`: when a plugin's `process()` returns `-2` (SEH crash / throw), the proxy deliberately does **not** release the input doc ref that `share_out` reserved for the adopting side — the comment (`:570-571`) explains it declines to risk a double-release against a torn call that may or may not have adopted. That is the safe choice against a UAF, but the consequence is a permanent one-ref leak of a host-owned `yyjson_mut_doc` (and its pooled chunks) on **every** plugin crash. **Failure scenario:** a plugin that faults once per N frames (a real production degraded mode, and precisely the mode `on_fault=reinit`/`refuse` exists to manage) leaks a doc per fault indefinitely; over a long line shift this is unbounded host memory growth on the exact path already under stress. It is a defensible trade at the point of the bug, but it is a real, uncapped leak, and I flag it because the fix belongs in the *design* (see §4: I remove the shared-mutable-doc model that creates this dilemma).

### 1.5 Two things I checked that are NOT bugs (so the check is honest)

- The `ImagePool::release_all_for` owner sweep (`xi_image_pool.hpp:311-341`) drops exactly one ref and orphans (not frees) still-referenced entries — I traced the cross-instance caching case (a `buffer_replay`/gathering consumer holding a producer's handle) and the accounting is correct; the older force-delete would have been a UAF and the comment documents precisely that history.
- The `emit_record` door forwarder (`xi_image_pool.hpp:609-640` + `xi_trigger_bus.hpp:280-286`) does resolve the "door entry permanently null" landmine it describes; `door_matches_fields` (`:698-740`) asserts the published slot equals the wired field. It works. My objection to it is architectural, not a correctness bug — see §3.

**Net:** four real findings, two of them (1.1, 1.2) doc/gate-integrity issues that undermine trust in the safety net rather than the runtime, one shipped-example race (1.3), one design-rooted leak (1.4). No hot-path corruption. This is a mature system.

---

## 2. The thesis

Read the findings again as a set. 1.1 is prose that mirrors behavior and drifted. 1.2 is a baseline/gate that mirrors the wire and can silent-skip. 1.4 is metadata held in two places (a shared mutable DOM refcounted across a DLL boundary) whose ownership dilemma has no clean answer. And the single largest chunk of complexity in the whole codebase — the ABI's `get_interface` door carved to be **byte-for-byte identical** to the struct fields, proven equal at startup by `door_matches_fields`, bridged by a forwarder slot because layering forbids the direct copy — exists solely to maintain *two representations of one function table.* The corroborating client investigation found the same shape again: `RunOutcome` is hand-reimplemented in four clients while a generated type sits unused; the command vocabulary is duplicated across four clients with no schema; the compat table drifts from the compat matrix by design.

Every one of these is the **same failure mode**: one fact, two representations, kept in agreement by human discipline and a guard test. The current code fights this heroically and mostly wins — but the wins are expensive (freeze-guards, baseline gates, `_io.h` conventions, four hand-patched clients) and the losses are exactly where the discipline lapsed (the stale README hedge, the silent-skip gate, the drifted `blob_analysis` typed view).

**xInsp3's governing law: one owner per fact; every other view of that fact is a *generated projection*, never a hand-synced mirror.** The current code already discovered this law in one place and named it beautifully — the health registry stores only facts "no other core owner already holds" and *derives* the rest at query time (`xi_health.hpp:8-21`, and note it explicitly cites "the exact hand-synced-representation failure the v3 architecture exists to kill"). I am promoting that from a tactic in one header to the spine of the whole system.

The rest of this document is that law applied, plane by plane.

---

## 3. The plugin boundary: a query-door ABI, and *only* the door

**Keep the C ABI. Keep in-process, trusted-load, refcounted handles. Delete the monolith entirely.**

The stable C ABI is the right boundary and non-negotiable for MSVC-drift survival — I keep it verbatim in spirit. What I delete is the `xi_host_api` **struct-of-function-pointers** as an access path. The current code lived a whole saga (v1 monolith → append-until-v9-freeze → v10 adds the `get_interface` door → v11 "大破大立" break to remove five dead SHM stubs and retire the whole-table legacy view, `xi_abi.h:135-161`). The end state is *two* parallel ways to reach every capability — the struct field and the carved interface — that a startup guard must prove identical, plus a forwarder-slot bridge for the one field (`emit_record`) that couldn't be copied at build time (`xi_image_pool.hpp:592-640`).

xInsp3 ships **only** `get_interface(id, version) -> const void*` from line one. Every capability is a small, independently-frozen struct behind that door: `xi.imaging@1`, `xi.imaging_rw@1`, `xi.doc@1`, `xi.emit@1`, `xi.log@1`, `xi.preview@1` — the exact carving the current code already arrived at (`xi_abi.h:230-318`), but as the *sole* representation.

What this buys, directly from the findings:
- **No `door_matches_fields`, no freeze-guard, no forwarder slot, no layout `static_assert`s.** They all exist to keep two representations equal (`xi_abi.h:546-569`, `xi_image_pool.hpp:690-740`). With one representation there is nothing to keep equal. The `XI_ABI_EXPECTED_SIZE` / `offsetof` guard machinery evaporates.
- **No min-compat break to remove a field.** Retiring a capability is "stop answering that `(id, version)`" — a query returning null, which every caller already null-checks. The current code needed a *major authorized ABI break* (`XI_ABI_MIN_COMPAT` 6→11) just to delete five null pointers from a struct. In xInsp3 that is a one-line registry edit with zero binary-layout consequence.
- **Versioning becomes local.** `xi.imaging@2` coexists with `@1`; a plugin asks for what it can use. No global ABI integer that gates every plugin's load against every capability's evolution.

The single ABI version integer survives only as a coarse "is this plausibly one of us" handshake; capability negotiation is per-interface. I keep the two optional plugin exports the current design got right — staged config swap (`xi_plugin_prepare`/`commit`, resolved by name, `xi_abi.h:710-716`) and the declared record schema (`xi_plugin_record_schema`, `:718-731`) — because both are already additive-by-construction and don't touch the door. The record schema I make **mandatory and generative** (§5).

**Divergence stated plainly:** the current code treats the monolith struct as the load-bearing surface and the door as an additive escape hatch layered on top. I invert it: the door is the *only* surface, and there is no struct to escape from. This is my sharpest disagreement with the current design's direction, and it deletes more code than any other single decision here.

---

## 4. The data plane: keep image zero-copy verbatim; redesign the metadata channel

### 4.1 Images — keep, almost verbatim

The `ImagePool` is the crown jewel and I would reimplement it nearly line-for-line: fixed-slot flat array, 16-bit slot + 40-bit generation handle, lock-free lookup/addref/release, the Treiber free-list with a packed version counter against ABA (`xi_image_pool.hpp:848-888`), the 1 GiB per-image sanity cap (`:130`), and — critically — the `WalkGuard`/`retired_` deferred-reclamation handshake so diagnostic slot walks never dereference a freed entry (`:780-846`). This is the correct shape for "no alloc on the per-frame path, zero-copy by handle, safe under a `TerminateThread`'d worker." I keep the owner-ledger sweep (`ImagePoolOwnerScope`, `:904-930`) as the leak backstop for instance-death, because I traced it and it's correct.

The one thing I'd add: the `xi.imaging_rw@1` read-only-input / writable-output discipline (`xi_abi.h:254-283`, `writable_data` returns null for refcount>1) should be the **default and only** pixel accessor from day one, not a later-carved corrective interface. The always-mutable `image_data` pointer is a footgun the current code had to add a whole interface to fence off after the fact; xInsp3 never ships the footgun. `image_read` returns `const uint8_t*`; `image_write` returns `uint8_t*` only for a uniquely-owned handle; there is no third "just give me a mutable pointer to anything" call.

### 4.2 Metadata — redesign away the shared mutable DOM

Here I diverge hard. The current metadata channel is a host-owned `yyjson_mut_doc*` passed by pointer across the DLL boundary, with a full parallel refcount system mirroring the image pool: `doc_chunk_*` allocator, `DocRegistry` with `doc_retain`/`doc_release`/`doc_refcount`, `share_out`/`adopt_shared`, and a frozen-vs-copy-on-write protocol keyed on whether `doc_refcount > 1` (`xi_abi.h:285-298`, `xi_use.hpp:541-601`). This is impressive machinery, and it is the source of finding 1.4 (the every-crash leak) and a large share of the ABI's conceptual weight. Its entire justification is avoiding one JSON serialize on the metadata channel.

My judgment: **for images, zero-copy is life or death — megabytes per frame. For metadata JSON — bytes to low kilobytes of routing/context — a shared mutable cross-DLL DOM with its own refcount registry is buying microseconds at the cost of the hardest ownership problem in the codebase.** The cost/benefit that is obviously right for pixels is not obviously right for a recipe id and a lane hint.

xInsp3's metadata channel is a **flat, typed, immutable value buffer** — a compact key→(type, bytes) table built by the emitter, serialized once into a pool-backed byte block, and handed across the ABI as `(const uint8_t*, len)` with a tiny reader. No shared mutability, so no refcount registry, no retain/release, no COW, no frozen flag, no "who releases on a torn call" dilemma — the buffer is copied into the trigger event once (it is small) and released with the event, full stop. The `Record`'s **image** bag stays zero-copy by handle exactly as today; only the JSON-DOM-by-pointer path is replaced. If profiling ever shows the metadata copy matters (it won't at kilobytes), the pool-backed block can be refcounted like an image with the *same* proven machinery — but I refuse to pay that complexity up front for a channel that doesn't need it. This directly dissolves finding 1.4: there is no reserved-ref-on-crash question because there is no reserved ref.

**The schemaless `Record` as the runtime container stays** — path expressions (`rec["items[0].score"]`), safe defaults, the named-image bag. It is a good ergonomic surface. What changes is that its **cross-plugin contract** is no longer "trust me, it's JSON" — see §5.

---

## 5. Typed I/O: the declared schema is the source; the accessors are generated

The current `_keys.h` / `_io.h` pattern (typed key constants + a chainable `Input` builder + an `Output` extractor) is a genuinely good idea for *consumers* — a key typo becomes a compile error. But it is a **hand-maintained mirror**, and the mirror has already drifted: `blob_analysis`'s `process()` emits a per-blob `contour` array that its own `Output::Blob` typed view has no accessor for (per the plugin investigation — the producer and the typed view are separately maintained and already disagree). That is finding-shaped: one fact (the record contract), two representations (the producer's hand-built output and the `_io.h` view), kept in sync by nobody.

xInsp3 makes the **declared record schema the single source**. The `xi_plugin_record_schema` export (`{"produces":[...],"consumes":[...]}`, `xi_abi.h:718-731`) — optional and purely declarative today — becomes **mandatory**, and:
1. The typed `Input` builder and `Output` extractor are **codegen'd from it**, not hand-written. Producer and view cannot drift because the view is a projection of the same declaration the producer is validated against.
2. It is **checked at load time**: a pipeline where plugin A's declared `produces` doesn't cover what plugin B `consumes` fails to arm, with a named error — a wire/graph contract violation caught before the line runs, not on frame N.
3. It feeds the protocol contract (§6) so the wire schema for a plugin's output is *derived*, not separately authored.

This is the same "generate the projection, never hand-sync the mirror" move as §3, applied to the Record contract. The runtime stays schemaless-and-fast (yyjson-backed `Record`, zero-copy images); the *contract* is typed, single-sourced, and enforced at the cold boundary where enforcement is free.

---

## 6. The protocol and contract discipline: one IDL, generated everywhere

The current state (from the client investigation, which I corroborated against the code): a real `contract/` with JSON schemas, a `baseline_gate.py` that genuinely bites on wire-shape drift, and a codegen — but **four parallel client implementations** (VS Code extension, `ui-components`/HMI, Python) that each hand-roll transport, envelope parsing, the `RunOutcome` band logic, and the command vocabulary. The generated types are imported only by a test and a compile-probe. A protocol change touches N clients and the schemas don't even cover the command names.

This is the thesis at the fleet scale: one wire contract, five-plus representations, synced by hand and alarmed (not prevented) by a gate that itself can silent-skip (finding 1.2).

xInsp3 has **one interface-description file** — the commands, the events, the payload shapes, the run-outcome bands, all of it — and **everything else is generated from it**:
- The backend command dispatch table (handler signatures, arg validation).
- The TypeScript client type layer that the VS Code extension **and** the HMI **and** `ui-components` all import (not a vendored bundle that lags — a generated package).
- The Python client.
- The JSON schemas used by the gate.
- The run-outcome class/band logic (`XI_SYS_*`, `outcome_class_for_code`) — currently reimplemented in `service_result.cpp:110-118`, `runner_main.cpp:189-196`, `runOutcome.ts`, and `client.py` *independently*. Generated once, imported four times.

**The gate changes character too.** Today `baseline_gate.py` bites but `validate.py` silent-skips on a missing dependency (finding 1.2). In xInsp3 the contract check runs at **build time as a codegen-diff**: regenerate all projections from the IDL, `git diff --exit-code`. If a client's checked-in generated file differs from what the IDL produces, the build fails — no runtime dependency to be absent, no "SKIP" path, because "the projection is stale" is a textual diff, not a jsonschema validation that needs a package installed. A gate that cannot silent-skip is the only kind worth having.

I keep the **compat manifest** idea (`tools/compat_manifest.mjs`) — a self-describing artifact stamped from real per-package sources so a target machine can answer "what am I running" — because that is a genuinely good production affordance and it reads from real sources rather than hand-typed values. I delete the hand-maintained README compatibility *table* that the manifest test only `console.warn`s about; the table is a mirror, so it too becomes generated from the matrix or it doesn't exist.

---

## 7. State, health, and lifecycle ownership: the model is already right — enshrine it

`xi_health.hpp` is the best-designed component in the codebase and I keep its principle **verbatim as law**: store only the facts no other owner holds (top-level state, script component health, the per-instance runtime-fault overlay), and **derive** everything else — instance base state from the PluginManager's `InstState`, group state from the lanes, source liveness from the TriggerBus emit ages — *at query time* (`xi_health.hpp:8-21`). One owner per fact; the aggregate is a projection. This is precisely the thesis, and the codebase discovered it here first.

I keep the fault-policy vocabulary (`OnFault::{Reuse,Reinit,Refuse}`, `xi_fault_policy.hpp`) — a stateful plugin caught mid-mutation carrying corrupt state into the next frame is a real hazard, and `reinit` (tear down + rebuild from last committed config) with escalation-to-`refuse` after `kReinitEscalateAfter` failures is the correct three-way answer. This is not deletable; it is a genuine functionality requirement realized cleanly.

I keep the lifecycle machinery that the audit rounds clearly paid for in blood:
- `controlled_shutdown_teardown_` as the **single source of truth** for teardown ordering (`service_dispatch.cpp:426-476`) — stop emitters, drop the sink, unload script under lock, drop `srv` last — with the hard-exit-on-wedged-drain trade (`:448-462`). Hand-copied teardown was a documented recurring bug; one function is the fix.
- The `InflightRuns` Dekker handshake (`xi_inflight_runs.hpp`) so a detached one-shot racing teardown is either counted by the drain or bails. This is the shape that kills the shutdown-window UAF class, and I keep it structurally (the protocol lives in one type, not copied at each launch site).
- The `DispatchPoolGuard` RAII quiesce/resume so a hot-recompile can't silently leave the stream stopped (`service_dispatch.cpp:557-603`).

The lesson these encode — *make the correct ordering a property of a type, not of every caller remembering* — is the RAII cousin of the single-source law, and I adopt it as a second-order rule.

---

## 8. The runtime topology: keep the FE/BE split; make the WS server multi-client

**Keep:** the supervisor/compute split (`xinsp-fe.exe` spawns and respawns `xinsp-backend.exe`, records crash history), in-process plugins (a hard plugin crash takes the BE down; the FE respawns — the accepted, correct trade for zero-copy speed), SEH→C++ per-worker translation (`xi_seh.hpp`), and the crash-safety-is-a-plugin's-sidecar decision (a comms/PLC plugin spawns its own process to watch the BE handle and go line-safe on death — `xi_abi.h:382-385` records that `set_safe_state` was removed for exactly this reason; that is the *right* minimal-core call and I keep it).

**Redesign:** the single-WS-client lock. The current `xi_ws_server` refuses a second client with `503 single-client-busy` (per the client investigation). That forces the plugin webviews in VS Code to bridge through the extension host's one socket (`postMessage` → `exchange_instance` relay) while the HMI is expected to open its own — so a plugin author supports **two transports for one UI**, and a dev laptop + an operator HMI + a Python probe cannot observe the same running line simultaneously. For a production inspection station this is backwards: the operator HMI and a remote diagnostic client *must* coexist.

xInsp3's WS server is **multi-client with server-side subscriptions** from day one. The subscription/fan-out logic already exists — it just lives in the wrong place: the `expose` plugin tracks per-channel subscribers itself and gates `emit_binary` on them (per the plugin investigation). I pull that up: the **server** owns subscriptions (per-channel, per-client), and `emit_binary`/events fan out to subscribed clients. Plugins push once; the server routes. One transport for every UI (webview or browser, identical), N observers, per-client backpressure. This also removes the awkward in-plugin subscription state that today makes `expose` call a host WS primitive while holding its own lock.

**Keep the headless runner** (`xinsp-runner.exe`) as the production face — no WS, no UI, project-in → JSON-report-out — and keep its now-correct verdict capture (§1.1). One clarification I'd bake into the design: the report's **exit code stays an execution/infra status, and the verdict roll-up lives in the artifact** (`counts{}`). That split is correct (a line-fail policy is the integrator's, not the runner's) and I keep it — but I'd fix the README to stop denying the artifact exists.

---

## 9. The script experience: C++ JIT stays; the ambient trigger seam goes

**Keep C++-compiled-to-DLL scripts.** For a hot-path vision framework, "your script is real C++ with real OpenCV, JIT-compiled by `cl.exe` and hot-reloaded" is the right speed/expressiveness trade, and the hot-reload-with-state story (`xi::state()` serialize-before-unload) is a genuine HDevelop-class iteration loop despite the language. I would not trade this for an interpreted DSL; the whole point is zero abstraction penalty at the pixel.

**Delete the ambient thread_local trigger path.** The current code has two ways for a script to read the current inspection event: the legacy ambient `thread_local` via the `g_trigger_*_fn_` thunks, and the explicit `XI_INSPECT_ENTRY(t, frame)` / `xi_trigger_view` path that hands the trigger in by value (`xi_use.hpp:34-70, 152-197`). The code itself documents that the ambient seam is "the root of Problem A" — a worker thread that calls `current_trigger()` reads a null `thread_local` and *silently gets nothing* (`xi_use.hpp:35-40`). The explicit view is strictly better: self-contained, valid on any thread, safe to capture by value into `xi::async`/`parallel_for`. xInsp3 ships **only** the explicit entry. `current_trigger()` as an ambient global does not exist; the trigger arrives as a parameter, and a parallel body captures a `TriggerSnapshot` by value. This deletes a documented silent-failure footgun and one of the two representations of "the current event."

I also **delete the `VAR`/`EMIT` compile-but-no-op macros** (`overview.md:124` — they still compile but publish nothing; per-run surfacing is the `expose` plugin's job). A macro that compiles and silently does nothing is a trap for exactly the author following an old example. Per-run value/image surfacing is `xi::use("expose").process(rec)`, one way, and the guides say so.

**Keep `expose` as a plugin, not a core feature** — this is the philosophy working correctly. Surfacing intermediates for the UI is domain behavior; it rode in core as `VAR`/`EMIT`, and moving it to a shipped plugin (`expose`) that owns its own WS framing (the `XEX1` self-describing binary format shared by the plugin, the fixture test, and the JS/Python decoders from one encoder header) is the "can this be a plugin? then it must be" test applied and passed. I keep that verbatim, including the single-source-of-truth encoder — note that `expose`'s one genuinely shared encoder is itself an instance of the thesis done right.

---

## 10. The testing/gate philosophy: prevent drift, don't alarm on it

The current gate suite is unusually self-aware — every gate carries a "what regression this caught" preamble, and `doc_coverage` / `retired_terms` / `baseline_gate` / the compat-matrix self-check genuinely bite. I keep those. My changes are surgical and follow the thesis:

1. **No gate may silent-skip on a missing dependency** (finding 1.2). Any check that needs a package either has that package provisioned in the one CI job, or is reframed as a codegen-diff that needs nothing but `git`. The contract check becomes "regenerate all projections from the IDL and diff" (§6) — unskippable by construction.
2. **Prefer prevention to detection.** `baseline_gate.py` *detects* wire drift after the fact and asks the author to refresh a baseline honestly. A generated wire layer means most drift *can't happen* — the clients are projections of the IDL, so they change when it changes. The baseline gate survives only as a second-line breaking-change classifier.
3. **Kill the mirror gates.** The compat README-table check that only `console.warn`s (per the client investigation) is guarding a hand-typed mirror; delete the mirror (generate the table) and the gate has nothing to warn about.
4. **Keep the quarantine ledger honest.** `run_qa.py`'s known-failing quarantine self-heals against rotting-green (an unexpected pass is fatal) — I keep that direction; a `flaky` entry that suppresses both directions is a hole I'd narrow, but the mechanism is sound.

The philosophy in one line: **a good gate makes a class of bug impossible; a mediocre gate notices it later; a bad gate advertises a safety it silent-skips.** Move every check up that ladder.

---

## 11. Keep / Redesign / Delete — the ledger

**Keep verbatim (with the mechanism):**
- The lock-free `ImagePool`: generation/ABA handle, Treiber free-list with version counter, `WalkGuard`/`retired_` deferred reclamation (`xi_image_pool.hpp`).
- SEH→C++ per-worker translation (`xi_seh.hpp`) + the compute/emit split with the `EmitTurn` ordered-emit gate (`service_inspect.cpp`, `xi_emit_gate.hpp`).
- Single-source teardown ordering (`controlled_shutdown_teardown_`) and the `InflightRuns` Dekker drain (`xi_inflight_runs.hpp`); `DispatchPoolGuard` RAII quiesce/resume.
- The trigger-as-pure-funnel collapse (v6): multi-camera sync is a gathering plugin emitting one record with N images; replay is a buffer-replay plugin (`xi_trigger_bus.hpp:1-16`). This is the philosophy's best win — a whole correlation-policy subsystem deleted and re-expressed as plugin composition.
- The health registry's derive-don't-duplicate law (`xi_health.hpp:8-21`) and the fault-policy vocabulary (`xi_fault_policy.hpp`).
- C++ JIT scripts with stateful hot-reload; `expose` as a plugin with its shared `XEX1` encoder; the FE/BE supervisor split with in-process plugins.

**Redesign (with the concrete replacement):**
- **ABI:** one `get_interface(id, version)` query-door, no monolith struct → deletes `door_matches_fields`, the forwarder-slot bridge, the layout `static_assert`s, and the whole freeze/break saga (§3).
- **Metadata channel:** flat typed immutable value buffer passed as `(bytes, len)` → deletes `doc_chunk_*`/`DocRegistry`/`doc_retain`/`release`/`refcount`/`share_out`/`adopt_shared`/COW and dissolves finding 1.4 (§4.2).
- **Typed I/O:** `_keys.h`/`_io.h` codegen'd from a mandatory declared record schema, validated at load (§5).
- **Protocol/clients:** one IDL → generated backend dispatch + TS + Python + schemas + run-outcome bands; contract gate becomes an unskippable codegen-diff (§6).
- **WS server:** multi-client with server-owned per-channel subscriptions; one transport for every UI (§8).

**Delete:**
- The `xi_host_api` struct as an access path (keep only the door).
- The ambient `thread_local` trigger seam and `current_trigger()` as a global (keep only the explicit `xi_trigger_view` entry) — deletes a documented silent-failure footgun.
- The `VAR`/`EMIT` compile-but-no-op macros.
- Hand-maintained mirrors: the four parallel client protocol layers, the README compat table, the `blob_analysis` hand-written `_io.h` — each replaced by a generated projection.
- Fix (not delete) the README runner-verdict hedge (finding 1.1) and the `synced_stereo` `fps_` race (finding 1.3).

---

## 12. The one thing

**Bet the next generation on a single law and enforce it mechanically: one owner per fact; every other view is a generated projection, never a hand-synced mirror.**

I arrived at this not from taste but from the findings. The current system's runtime is nearly flawless; its *residual* risk is concentrated with eerie precision in one shape — a fact represented twice and reconciled by discipline. The stale README hedge, the silent-skip schema gate, the four hand-rolled clients, the drifted `blob_analysis` typed view, the every-crash doc-refcount leak, and the single largest lump of ABI complexity (a door proven byte-for-byte equal to a struct it duplicates) are all the *same bug wearing six costumes*. And the codebase already wrote the cure into its own best component: `xi_health.hpp` stores only what no one else owns and derives the rest, explicitly naming "the hand-synced-representation failure the v3 architecture exists to kill."

xInsp3 promotes that from one header to the constitution. The ABI is only the query-door (no struct to keep in sync). The wire is only the IDL's generated projections (no client to hand-patch). The typed Record views are only codegen from the declared schema (no `_io.h` to drift). The health aggregate is only a query-time derivation (no mirror to reconcile). And the gates enforce it as textual codegen-diffs that *cannot silent-skip*, because a stale projection is a `git diff`, not a validation that needs a package to be installed.

The speed-first, minimal-core, functionality-as-plugins spine stays exactly as written — this law is how you *keep* it true at scale. Every heroic freeze-guard and baseline gate the current team wrote to survive duplication is effort they shouldn't have had to spend. Spend it once, at the source, and the mirrors take care of themselves.
