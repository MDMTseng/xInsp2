# The Frame Owns Everything In It — A Next-Generation xInsp Architecture (Vision A)

| | |
|---|---|
| **Date** | 2026-07-02 |
| **Author** | independent architect A |
| **Basis** | `polaris_master` @ HEAD (the most hardened xInsp2 state) |
| **Method** | Read the code, not the plans. `docs/new_gen/` and `docs/ext_review/` deliberately unopened — this is an unanchored second opinion. Every criticism cites `file:line`; every praise names the mechanism. |

---

## 0. Reading notes

I read the spine directly: the frozen C ABI (`backend/include/xi/xi_abi.h`), the lock-free image pool (`xi_image_pool.hpp`), the yyjson-backed `Record` with its γ-4 cross-ABI doc sharing (`xi_record.hpp`), the dispatch funnel (`xi_trigger_bus.hpp`), the script-side instance proxy and explicit-trigger view (`xi_use.hpp`), the single-frame inspection cycle (`backend/src/service_inspect.cpp`), the SEH boundary (`xi_seh.hpp`), the headless runner (`backend/src/runner_main.cpp`), and the plugin builder/extractor pattern (`plugins/blob_analysis/blob_analysis_io.h`). I surveyed the client fleet and the contract/gate machinery in breadth.

My headline reaction: **this is a genuinely excellent one-machine machine-vision core.** It is not a toy, and most of the "obvious" criticisms I went looking for have already been found and fixed with visible scar tissue. The interesting design question is therefore not "what is broken" but "where has the pursuit of zero-copy sharing bought so much refcounting machinery that a greenfield could get the same speed with a fraction of the moving parts." That is the argument of this document.

---

## 1. Findings from the check

I found no showstopper race in the core — the pool and the Record are carefully reasoned. I found one real doc-vs-code lie, one gate that is red-and-ignored on HEAD, one structural gap in the contract's authority, and two sharp edges worth naming. I am not padding; these are what I actually found.

### 1.1 The README says the runner can't do what the runner does (doc-vs-code lie)

`README.md:288-297` (usage step 9) states, in a blockquote, that the headless runner report *"does **not** yet capture the per-frame inspection **verdict** (OK / NG / NA)"* and that *"Per-frame verdict capture (wiring the result callback) is **planned but not yet implemented**."*

That is false. The same README's Features section (`README.md:435-442`) says the opposite — the report *"carries both the execution/crash log … and the per-frame verdict"* — and the code agrees with Features, not step 9: `runner_main.cpp:438-440` wires `script.set_result_callback((void*)result_cb)`; `:528-534` derives the verdict class and tallies it; `:543-549` writes `counts{ok,ng,na,no_verdict,crashed}` into the summary. The header comment `runner_main.cpp:11-18` documents the implemented behavior explicitly.

**Failure scenario:** a factory integrator reading step 9 concludes the JSON report is verdict-blind and builds an external pass/fail scraper they did not need, or worse, distrusts the `counts` block that is actually authoritative. The README contradicts itself in two places about a production-facing artifact. Fix: delete the stale blockquote at `:288-297`.

### 1.2 The strict contract gate is RED on committed HEAD

`contract/validate.py:100-104` treats any fixture not mapped in `contract/fixtures-map.json` as a hard failure (*"a new fixture cannot land unvalidated"*). Four fixtures are committed at HEAD with no mapping and no backing schema: `protocol/fixtures/get_health.json`, `health_changed.json`, `health_changed_state.json`, `hello.json` — there is no `health` schema under `contract/schemas/`. Running `validate.py` exits 1 today. The gate is wired as the `contract_schema` ctest (`backend/CMakeLists.txt` ~`:960`) and is invoked by `tools/gate.py`, which `.github/workflows/ci.yml` runs on every push/PR.

**Failure scenario:** this is simultaneously proof the gate *bites* and proof it *was bypassed* — commit `2c409fd` (health surfacing) landed without anyone running ctest/CI to green. A gate that is chronically red is a gate everyone learns to ignore, which is worse than no gate. Either add the `health` schema + fixture mappings, or the health work never truly passed the discipline the repo claims.

### 1.3 The "contract" has no automatic tie to the actual C++ emitter

The schemas are hand-authored and self-describe as *"Descriptive of the CURRENT wire"* (`contract/schemas/run-outcome.schema.json:5`). Codegen runs **schema → types** (`contract/codegen/gen_types.py:8`), never source → schema. The only thing linking wire bytes to schema is a set of hand-written golden fixtures; there is no test that connects a running backend, captures a real frame, and validates it against the schema. So the integrity chain is *hand-written schema ↔ hand-written fixture ↔ SDK parser* — three hand-maintained mirrors. A C++ change to the emitted bytes that skips updating the schema and the fixture passes every gate green.

This does not contradict any single README sentence outright, but `README.md:559-570` presents the contract as the machine-checked authority, and that impression is stronger than the mechanism supports. The baseline gate protects schema↔baseline; nothing protects schema↔emitter.

### 1.4 SEH stack-overflow translation leaves a holed stack (lower confidence)

`xi_seh.hpp:40` translates `STACK_OVERFLOW (0xC00000FD)` into a C++ `seh_exception` via `_set_se_translator`, and `service_inspect.cpp:192-202` catches it and keeps the dispatch worker thread alive for subsequent frames. On Windows, a stack-overflow fault consumes the guard page; without `_resetstkoflw()` the page is never restored, so that worker thread runs all later frames on a shrunken/holed stack — and the C++ unwind for the exception object itself needs stack it may not have. The common single-overflow case usually survives (there is headroom), but a script that recurses to overflow leaves *that lane* fragile for the rest of the run.

**Failure scenario:** under `dispatch_threads > 1`, one script bug quietly degrades one worker's stack budget; a later, shallower recursion on the same worker faults uncatchably and takes the backend down — appearing as a random crash unrelated to the original overflow. Worth either resetting the guard page on this specific code or exiting the process on stack overflow (the watchdog path already prefers process-exit-and-respawn for hard trips, `service_inspect.cpp:113-116`).

### 1.5 The sole-image fallback masks key typos in the common case (sharp edge, by design)

`xi_use.hpp:254` (and `:416` for the snapshot) returns the *only* image for *any* key when the trigger carries exactly one image. Combined with the emitter keying a keyless image by source name (`xi_trigger_bus.hpp:175-180`), `t.image("typodname")` silently returns the frame in a single-camera setup. It is documented as intentional ("sole-image fallback"), but it means a script that would correctly break under two cameras passes under one — the failure only appears when a second source is added. This is a footgun, not a bug; I would make the fallback opt-in (`t.image()` with no argument for "the frame," named lookups strict).

---

## 2. What the current design got RIGHT — keep verbatim

Before I redesign anything, the parts I would copy into the next generation unchanged, because they are the hard-won correct answers:

- **Images as opaque refcounted handles behind a C ABI, never raw pointers across the boundary** (`xi_abi.h:167`, `xi_image_pool.hpp:83`). The generation-stamped 64-bit handle (16-bit slot + 40-bit generation) that fails `lookup()` cleanly on a stale handle (`xi_image_pool.hpp:101-111`) is exactly right — it turns a careless plugin's use-after-release into a clean null instead of a landing on the next occupant. Keep this handle scheme *verbatim*.
- **SEH → C++ exception translation wrapping every script/plugin call site** (`xi_seh.hpp`, `service_inspect.cpp:142-217`). A null deref in user code returning an error while the backend stays up is the single most important operational property of a "C++ scripts JIT'd to DLLs" system. Keep the boundary; fix only the stack-overflow corner (§1.4).
- **The crash breadcrumb + per-inspect watchdog with a monotonic deadline** (`service_inspect.cpp:117-135`), and the decision to *exit-and-respawn* on a hard watchdog trip rather than `TerminateThread` a worker (which would leak the per-instance lock). This is mature thinking.
- **The core-owned health registry and the honest run-outcome** — a run that set no verdict emits `no_verdict`, a crash emits `crashed`, neither is silently `NA` (`service_inspect.cpp:259-289`, mirrored honestly in `runner_main.cpp:189-196`). This honesty is recent and correct; do not regress it.
- **"Multi-camera sync is a gathering plugin, replay is a buffer-replay plugin, not a bus policy"** (`xi_abi.h:93-105`, `xi_trigger_bus.hpp:6-12`). Collapsing the dispatch to one verb — `emit_record` in, one `TriggerEvent` out — and pushing correlation into plugin composition is the purest expression of "the core is a dumb hub." Keep the funnel.

---

## 3. The next-generation architecture

Same philosophy — speed-first, minimal core, functionality-as-plugins, one machine, C++ JIT, WS + VS Code + HMI. Where I diverge from the current code, I argue it.

### 3.1 Data plane — **REDESIGN: one frame, one arena, one owner, one free.** (my boldest divergence)

This is where I part company with the current design most sharply, and I want to be explicit that I am arguing *against* a thing the current code does well, not around it.

The current data plane is built on **two independent refcount systems plus a copy-on-write value type**:
1. The `ImagePool` per-image atomic refcount, with owner-sweep, deferred reclamation for concurrent stats walks, and a Treiber free-list (`xi_image_pool.hpp` — ~930 lines).
2. The `DocRegistry` per-doc refcount for yyjson docs handed across the ABI (`xi_doc_registry.hpp`), enrolled via `share_out` / consumed via `adopt_shared` (`xi_record.hpp:207-252`).
3. `Record` itself, an intrusive-refcounted `DocBox` with a `frozen_` flag and `cow_()` copy-on-write, plus a `HostReleaseFn` that decides whether a doc frees via yyjson or routes back to the host (`xi_record.hpp:112-125, 752-771`).

The *reason* is zero-copy sharing: a producer's output doc can be cached by a downstream consumer across frames with no serialize and no deep copy. That is a real win for the specific plugins that cache (`buffer_replay`, accumulators, gathering). But look at the cost it imposes on **every** frame and on the reader of this code:

- Every `xi::use().process()` call does a CAS-guarded registry enroll, two `doc_retain`s, and a conditional `doc_release` on four different return paths (`xi_use.hpp:541-573`), with a comment block longer than most functions explaining which ref balances which.
- The owner-sweep contract (`xi_image_pool.hpp:281-341`) had to be *rewritten* once already because "owner is gone ⇒ free the entry" was false under cross-instance zero-copy sharing — it now drops exactly one ref and orphans still-referenced entries to anonymous owner. That's a subtle invariant maintained by hand.
- `xi_record_out_free` carries a cross-CRT free hazard that exists *only* because output strings can be malloc'd in the plugin DLL and freed in the host EXE (`xi_abi.h:640-675`), mitigated by routing through thread-local storage — machinery whose sole job is to survive the ownership model.

**My replacement: a per-frame arena.** Each dispatched inspection gets one bump-allocated `FrameArena` that owns *everything the frame produces* — pixel buffers, JSON nodes, verdict, staged sink payloads. The arena is handed to the script and to every plugin `process()` as a **borrowed `const` view**. Plugins allocate their outputs *into the same arena* (the host hands them an arena allocator, not `image_create`). At frame end the host frees the arena in one shot: no atomics, no free-list, no generation stamps, no owner-sweep, no DocRegistry, no `share_out`/`adopt_shared`, no COW.

- **Speed:** bump-alloc + one bulk free beats per-image atomic refcount churn and per-frame malloc, and it is cache-friendly (a frame's data is contiguous). This is *more* speed-first, not less — the current design pays atomic RMW traffic on the hot path to enable a sharing pattern most frames don't use.
- **The caching case pays explicitly.** A plugin that must outlive its frame (replay, accumulator, gathering across frames) calls `arena.retain_copy(handle)` to copy the bytes it needs into its own long-lived storage. The rare consumer pays a copy; the common consumer pays nothing. This inverts the current default (everyone refcounts so the rare cacher can be zero-copy) into the honest one (the cacher copies, everyone else is free).
- **Crash isolation improves.** When a frame faults, the host drops the whole arena — there is no "did the crashed plugin adopt the doc or not?" ambiguity that `xi_use.hpp:558-579` has to reason about. One owner, one lifetime, one free, even on the fault path.

What I keep from the current data plane: the **opaque handle** idea (arena slots still hand out generation-stamped handles across the ABI so a stale handle faults cleanly, §2), and **images-and-JSON-in-one-container** (the `Record` concept). What I delete: the two refcount registries, COW, and the frozen-doc dance. **This is the divergence I would bet the redesign on if forced to pick just one structural change.**

### 3.2 The plugin boundary — **KEEP the C ABI; REDESIGN the schemaless Record into a checked, generated contract.**

Keep, verbatim in spirit:
- **A pure-C ABI with no C++ types across `xi_plugin_*`** (`xi_abi.h:704-716`). This survives MSVC drift and is non-negotiable for a JIT-DLL system.
- **The builder/extractor `_io.h` pattern** (`blob_analysis_io.h`) — a typed `Input`/`Output` façade over a schemaless `Record` so a key typo becomes a compile error. This is genuinely elegant and I would make it the *only* sanctioned way a script talks to a plugin.

Redesign:
- **The schemaless `Record` stays on the wire, but the plugin-to-plugin contract becomes mandatory and load-gated.** Today `xi_plugin_record_schema` is optional (`xi_abi.h:718-731`) and the `_io.h` headers are hand-written with a "guard 4" comment promising stage-2 codegen will regenerate them (`blob_analysis_io.h:29`). In the next gen, **the `_io.h` header is generated *from* the plugin's declared schema, not hand-maintained alongside it**, and the schema export is required. At project load, the host cross-checks every `produces`/`consumes` edge in the script's plugin graph and refuses to arm a pipeline whose producer/consumer key types disagree. The schemaless freedom stays for *script authoring* (fast, exploratory); the *composition* is checked. This directly attacks the gap in §1.3 at the ABI level, not just the wire level.
- **Collapse the monolith→carved-interface duality by starting carved.** The current ABI keeps *both* the flat `xi_host_api` struct fields *and* the `get_interface`-carved `xi.imaging/doc/emit/log@1` interfaces, asserted byte-for-byte identical by a freeze-guard (`xi_image_pool.hpp:698-740`). That belt-and-suspenders is the right *migration* for a frozen ABI with live plugins, but a greenfield has no live plugins. Ship **only** `get_interface` from day one; there is exactly one way to reach a capability, the freeze-guard's job shrinks by half, and `door_matches_fields` disappears. Keep the CLAP-style capability query (`xi_abi.h:487-516`) — it is the correct evolution primitive — just don't also keep the thing it was carved out of.

### 3.3 Script experience — **KEEP C++ JIT; DELETE the ambient thread_local entry; REDESIGN cold-start.**

C++ scripts compiled to DLLs by `cl.exe` is the soul of the product and I would not touch it. But:

- **Make the explicit-trigger entry the only entry; delete the ambient path.** `xi_use.hpp:34-52` documents that the legacy `xi_inspect_entry(int)` reads the trigger from an ambient `thread_local` and is *"the root of Problem A"* — a worker thread that calls `current_trigger()` silently gets nothing. The A4 cure (`XI_INSPECT_ENTRY(t, frame)` + the self-contained `xi_trigger_view`, `xi_use.hpp:674-681`) is already the correct design: everything the script needs travels in the struct, valid on any thread, capturable by value into `xi::async`. A greenfield deletes the thread_local thunks and the `Trigger::ensure()`/`loaded_` fallback machinery (`xi_use.hpp:373-383`) entirely. One entry, no ambient state, no "valid only on the inspect thread" caveat.
- **Warm-compiler pool.** The single biggest UX tax on "edit → hot-reload" is `cl.exe` cold start. Keep the one-file recompile, but run a small pool of pre-warmed compiler processes (vcvars already sourced, PCH of `<opencv2/opencv.hpp>` + the `xi` umbrella already built) so a script edit is a *link*, not a cold `cl` invocation. This is pure latency, no semantic change.
- **Keep** `xi::Param<T>` sliders driving `set_param` with no recompile, and `xi::use("expose")` as the inspection surface. These are the right three primitives (`README.md:90-97`).

### 3.4 Protocol & contract discipline — **REDESIGN: generate the wire from one source; add a live-capture conformance test.**

The contract machinery is real and bites (§1.2 proves it), but it is a set of hand-maintained mirrors (§1.3). The next-gen fix is structural:

- **One source of truth: annotated C++ wire structs.** Declare each message shape *once*, in the C++ that emits it, with lightweight field annotations. Generate the JSON schema, the TypeScript types, the Python dataclasses, *and* the golden fixtures from that one declaration. `compat_manifest.mjs` already scrapes constants out of C++ with regexes to stamp the manifest — formalize that into real codegen so the schema can never lead or lag the emitter.
- **A live conformance test.** Stand up a real backend, drive one of every command, capture the emitted frames, and validate each against its generated schema. This is the missing link in §1.3 — the only test that proves *the actual bytes* match the contract. Wire it as a ctest so it runs where `contract_schema` runs.
- **Keep** the baseline gate's shape-diff classifier (`contract/baseline_gate.py:141-269`) — its breaking-vs-additive discrimination is good — but let it diff *generated* schemas, so a wire change forces a schema regen forces a baseline bump. Then the gate is unbypassable by construction, not by discipline.

### 3.5 State, health, lifecycle — **KEEP.**

The core-owned health registry with the `xi.health/1` schema, driven through its lifecycle by both the service and the runner (`runner_main.cpp:342-344, 490-497`), and the honest verdict classes (§2), are recent, correct, and minimal-core-shaped: health is a small primitive the core owns because nothing else can, and everything richer (analytics, history) is a plugin over the run-outcome stream. Keep this whole design. My only addition: since the frame arena (§3.1) makes a frame's full lifetime explicit, stamp the health transition and the verdict into the arena so the run-outcome is assembled from one place instead of read back from globals/thread-locals across the compute→emit seam (`service_inspect.cpp:46-54` currently threads a `RunOutcome` struct across that seam by hand — the arena subsumes it).

### 3.6 Client fleet — **REDESIGN: one generated transport per language; delete the divergence.**

Today there are three independent WS framing/dispatch implementations — TypeScript `WsClient` (`vscode-extension/src/wsClient.ts:12`), browser `XiClient` (`ui-components/src/ws-client.mjs:36`), Python `Client` (`tools/xinsp2_py/xinsp2/client.py:373`) — agreeing on the wire by hand, sharing no code, and diverging in auth (Python has bearer+hmac, TS bearer-only, browser none) and reconnect. Worse, the VS Code extension injects Microsoft `@vscode-elements` into plugin webviews while everything else uses `@xinsp2/components` — two webview UI toolkits in one product.

Next-gen:
- **Generate the request/response correlation + typed command verbs from the same contract (§3.4)**, per language. Hand-maintain only the thin socket transport. Then "add a command" updates one declaration and every client gets the typed verb; the auth/reconnect divergence becomes a single policy knob, not three re-implementations.
- **One webview UI kit.** The extension consumes `@xinsp2/components` + the shared client like the HMI does; delete the `@vscode-elements` injection path (`vscode-extension` webview hosting, ~`:2764`). The `postMessage`→`exchange_instance` bridge stays (plugin webviews still shouldn't hold their own socket), but they render with the same kit everywhere.
- **Keep** the single-controlling-WS-client topology and the `503`/busy-close (`BUSY_CLOSE_CODE 4003`) — one machine, one authority is right. Add a **read-only observer channel** (many viewers, one controller) as a *plugin*, not a core change, so the developer-in-VS-Code and the operator-at-the-HMI can both watch without fighting over the single client slot.

### 3.7 Testing & gates — **KEEP the gate.py funnel; REDESIGN toward generation + live capture.**

`tools/gate.py` as the one authoritative pre-merge command that CI runs, with ctest + node tests underneath, is the right shape. Keep it. The redesign is entirely §3.4's consequence: once the wire is generated and there is a live-capture conformance test, most of the hand-fixture maintenance (and the §1.2 red-gate class of failure) evaporates because you can't commit a wire change without regenerating. Also: **make the gate's own green state a merge precondition that is checked, not assumed** — §1.2 shows a red gate landed on HEAD, which means the gate ran nowhere that blocked the merge.

### 3.8 Runtime topology — **KEEP the split; KEEP in-process plugins + crash isolation.**

FE (`fe_main`) / BE split, in-process plugins for zero-copy speed, SEH crash isolation, single WS authority, headless runner as the production face — this is the correct set of trades for one machine, and the 2026-05 removal of process-isolation + SHM (`README.md:526`, the dead `shm_*` stubs finally deleted at ABI v11, `xi_abi.h:135-149`) was the right call. Keep all of it. The frame arena (§3.1) makes the in-process choice *safer*, not just faster, because a faulted frame's cleanup is a single free rather than a refcount reconciliation.

---

## 4. What I would DELETE outright

- The **ambient `thread_local` inspection entry** and its thunk fallbacks (`xi_use.hpp` `Trigger::ensure`, the `g_trigger_*_fn_` path). Explicit view only.
- The **`DocRegistry` + `share_out`/`adopt_shared` + `Record` COW/frozen machinery** (`xi_record.hpp:112-252, 752-771`) — subsumed by the frame arena.
- The **legacy flat `xi_host_api` struct fields** that duplicate the carved interfaces, and the `door_matches_fields` freeze-guard that exists to keep them in sync (`xi_image_pool.hpp:698-740`). Carved-only.
- The **cross-CRT `xi_record_out_free` dance** (`xi_abi.h:640-675`) — with an arena allocator, the plugin never mallocs output storage the host must free.
- The **second webview UI toolkit** in the VS Code extension.
- The stale **README step-9 blockquote** (§1.1) and the four **unmapped health fixtures** (§1.2) — housekeeping, but they are lies in the tree today.

---

## 5. The one thing

**The frame is the only contract, and it should own everything in it.**

xInsp2's deepest correct instinct is that an inspection is one event carrying images + metadata + a verdict, dispatched once, end to end. But it implemented the *sharing* of that event's contents with two refcount registries, a copy-on-write value type, an owner-sweep, and a cross-CRT free protocol — a great deal of carefully-correct machinery whose job is to let the rare caching plugin be zero-copy while making every frame pay atomic refcount traffic and making every reader of `xi_use.hpp` and `xi_record.hpp` reason about who balances which ref.

Collapse it. Give each frame a single bump-allocated arena that owns its pixels, its JSON, and its verdict; hand it to scripts and plugins as a borrowed `const` view; let outputs be written back into the same arena; free it in one shot at frame end. Make the *schema* of that frame a generated, load-gated contract rather than a hand-maintained convention. One owner, one lifetime, one free, one generated truth.

That single decision makes the hot path faster (bump-alloc beats atomic churn), the crash path simpler (drop the arena, no reconciliation), the ABI smaller (no doc registry, no COW, no cross-CRT free), and the contract honest (generated, not mirrored). Everything else in this document is an argument for keeping what xInsp2 already got right — the C ABI, the opaque handle, the SEH boundary, the dumb-hub dispatch, the honest health/verdict — around that one change.
