# xInsp3 — One Truth, Many Projections

A next-generation design for the xInsp machine-vision framework, argued from the code of `polaris_master`.

| | |
|---|---|
| **Date** | 2026-07-02 |
| **Author** | Independent architect D |
| **Basis** | `polaris_master` @ HEAD |
| **Method** | Read the SDK headers, the service composition, the plugins, the clients, the gates. Every criticism cites `file:line`; every keep names the mechanism. Did not read `docs/new_gen/` or `docs/ext_review/` — this is an unanchored second opinion. |

---

## Thesis in one paragraph

The current system is *mechanically* excellent and *representationally* over-extended. Its lock-free primitives, its teardown ordering, and its refcount discipline are the work of people who have been burned and learned. But almost every bug class the hardening rounds kept re-hitting has the same shape: **a second hand-maintained copy of a truth that already lives somewhere else, drifting from the original.** The instance-state map that had to be moved into the PluginManager; the capability door that had to be freeze-guarded against the struct field; the three hand-rolled WS clients; the three parallel descriptions of the wire; blob_analysis's typed `Output` that can't read the contour its own `.cpp` writes. The one place the codebase already internalized the cure — the health registry, which *stores only what no other owner holds and derives the rest at read time* (`xi_health.hpp:9-17`) — is the seed of the whole next generation. I would make "own each truth once, project it everywhere else, and gate the projection so it cannot skip" the organizing law of xInsp3, and I would pay for it by **deleting the schemaless mutable-JSON Record from the per-frame plugin boundary** — the single richest source of accidental complexity in the tree.

---

## Findings from the check

Real defects and doc/code lies I can stand behind, each with a failure scenario.

### 1. The schema contract gate silently does nothing on CI (a gate that does not bite)
`contract/validate.py` is the *only* check that validates the wire schemas as schemas and validates fixtures against them. It `sys.exit(0)` (SKIP, not FAIL) the moment `jsonschema`/`referencing` are unimportable (`validate.py:35-39`). CI installs only `pytest` (`.github/workflows/ci.yml`, "Install Python test deps" → `pip install --upgrade pip pytest`); `jsonschema` is in no `pyproject.toml`, no `setup-windows.ps1`. So on CI and on a standard local box the gate imports nothing, prints SKIP, returns 0. Its own docstring asserts the opposite — "already a repo QA dependency; used by tools/run_qa.py's environment" (`validate.py:18-19`) — which is false. **Failure scenario:** a contributor lands a schema that reaches for a banned `oneOf`, or a fixture with no schema mapping; the subset ban and the "no fixture lands unvalidated" strictness (`validate.py:11-16`) are both advertised and both unenforced; CI is green.

### 2. README's own walkthrough contradicts the runner it ships
README §9 (Usage walkthrough) states the headless report "does **not** yet capture the per-frame inspection **verdict** … Per-frame verdict capture … is **planned but not yet implemented**." The runner it describes captures verdicts fully: `runner_main.cpp:471-534` emits `{code, class, msg}` per frame with a `counts{ok,ng,na,no_verdict,crashed}` tally, and the file header says so explicitly (`runner_main.cpp:11-18`). README's *own* Features → Deployment section (further down the same file) also says verdicts are captured. **Failure scenario:** a user reads §9, believes exit 0 means "dispatched without crashing" only, and never learns the report already carries the pass/fail tally they need — the feature exists but the walkthrough hides it.

### 3. The exemplar's typed output cannot read what the exemplar writes
`blob_analysis` is one of only two plugins that fully implement the headline `_keys.h` + `_io.h` data-contract pattern. Its `.cpp` writes a `contour` array of `{x,y}` per blob, and the `_io.h` docstring advertises `[i].contour`. But `Output::Blob` exposes only `contour_points()` (a **count**), with no accessor for the points themselves. A script on the typed path can see how many contour points exist but must drop back to the raw `Record` to read them. **Failure scenario:** the pattern the SDK holds up as the gold standard is demonstrably a leaky veneer over the schemaless Record — which is the strongest possible evidence for finding this pattern hand-maintained and drift-prone (see Redesign §Plugin boundary). (Reported by exploration of `plugins/blob_analysis/*`; the count-only accessor is in `blob_analysis_io.h`, the point-writing is `blob_analysis.cpp:196-198`.)

### 4. A per-crash resource leak is baked in as the lesser evil
`xi::UseProxy::process` deliberately does **not** release the doc ref that `share_out` reserved for the adopter when a plugin call returns `-2` (crashed), to avoid a double-release on a possibly-torn call (`xi_use.hpp:558-579`, esp. the `-2 is left alone` comment). This is a defensible leak-over-UAF trade, but it means every SEH-caught plugin crash leaks one registry-managed `yyjson_mut_doc` plus its pooled chunks. **Failure scenario:** a plugin that faults on a hot input (a bad ROI every frame at line rate) bleeds doc-pool memory for the life of the process; the leak is invisible because it is on the rare path by design, but "rare" is a policy assumption, not a guarantee. This is not a coding error — it is the *cost of the design*: the γ doc-by-pointer machinery has a refcount contract so intricate that the only safe move on a torn call is to leak.

### 5. The shipped source plugins contradict the SDK's own blessed guidance
The expert template mandates `xi::spawn_worker` because "a stray fault on a raw `std::thread` with no translator brings down the whole backend," and mandates the wrapped `emit()`. Both real source plugins ignore both: `mock_camera.cpp:155` and `synced_stereo.cpp:74` spin raw `std::thread`s and call the lower-level free `xi::emit_record(...)`. **Failure scenario:** a fault on those grab threads is *outside* the SEH translator that `spawn_worker` installs per-thread — exactly the "takes the backend down" case the template warns against — and the canonical examples authors copy from teach the unsafe form.

### 6. Smaller true notes
- `lane_for_`'s comment claims routing is "never silently the front (#5)," yet its terminal fallback for an unknown group with an empty default snapshot is `return g_eng.lanes.front()` (`service_dispatch.cpp:118`). Benign, but the comment overstates the guarantee.
- `xi_protocol.hpp:5` names `protocol/messages.md` as the "canonical schema"; that file does not exist in the tree (only `protocol/fixtures/`). The C++ structs are in practice a third, ungated description of the wire alongside `contract/schemas/*.json` and the generated types.
- `data_output`'s `save` command is a counter bump that writes no file (`data_output.cpp:51-54`) despite a description promising CSV/JSON saving — dead/duplicate of `record_save`.

None of these are catastrophic. The pattern across them is the point: they are all **second copies drifting from a first**, or **a gate/doc claiming an enforcement it does not perform.**

---

## The next generation

### The organizing law: own once, project everywhere, gate the projection

xInsp3's core keeps the current spine verbatim — speed-first, minimal core, functionality-as-plugins — and adds one law that the current core only *partially* discovered:

> **No truth is represented twice by hand.** Every fact (a wire message shape, a plugin's I/O keys, the tested-together version set, a component's health) has exactly one authoritative owner. Every other place that needs it is a *generated projection* of that owner, and a gate that **cannot silently skip** fails the build if a projection drifts.

The health registry is the proof this works (`xi_health.hpp:9-17`): it refuses to store instance base-state, dispatch groups, or source liveness because those already have owners (PluginManager, the lanes, TriggerBus) — it derives them at `get_health` time, so they *cannot* drift. Generalize that from health to the whole system.

### Data plane — keep the images, delete the schemaless Record on the hot path

**Keep verbatim:** the `ImagePool`. Its handle layout (slot ⊕ generation, `xi_image_pool.hpp:101-111`), its Treiber-stack free list with ABA versioning (`acquire_slot_`/`release_slot_`, `:848-888`), and its deferred-reclamation `WalkGuard` that lets diagnostic slot-walks never dereference a freed entry without taxing the churn path (`:780-846`) are the best code in the repository. Zero-copy pooled images moving by refcounted handle is *correct* and I would not touch a line.

**Redesign — this is my boldest divergence.** The per-frame plugin boundary must stop carrying a schemaless, mutable, yyjson-backed `Record` across the DLL seam. The entire γ / γ-4 apparatus — `DocBox` with an atomic `host_release`, `share_out`'s CAS-claimed enroll plus a *reserved ref for the adopter*, `adopt_shared`, the copy-on-write on every setter, the cross-ABI JSON-fallback ref balancing that has to be re-derived correctly in `xi_use.hpp:540-579`, in `record_to_c` (`xi_abi.hpp:590-653`), and once more in `runner_main.cpp:119-122` — exists **only** to make passing mutable JSON by pointer fast enough for the hot path. That is a self-inflicted problem. The data crossing an operator boundary per frame is not a document; it is a **small, closed set of typed fields plus image handles.** So make *that* the type:

- **`xi::Frame`** — the hot-path record. A flat, arena-allocated (per-dispatch bump allocator, freed as one block at run end) map of `key → {tag, scalar | image_handle | bytes}`. Scalars are `int64/double/bool` inline; strings and blobs are arena slices; images are pool handles. No refcounted document, no COW, no cross-DLL `free`. Copy is a memcpy of POD; "cache across frames" is an explicit `handle.retain()` on the images you keep. The whole `xi_record.hpp` DocBox/frozen/cow_ machine (`:94-171, 207-252, 752-771`) and its three-site ref-balancing evaporate.
- **JSON lives only where it belongs:** config (`get_def`/`set_def`), the wire, and disk persistence. Serialization happens once, at the WS edge and the recorder, never per operator hop. This is exactly the split the ABI comment already wishes for ("Only the recorder serializes, and only when persisting to disk," `xi_abi.h:87`) — xInsp3 makes it structural instead of aspirational.

The throughput argument for the current design ("skip the JSON round-trip in-process") is real but it optimized the *wrong* representation. A typed arena `Frame` is faster than pooled yyjson *and* has none of the refcount surface. You do not need copy-on-write on a value type that is cheap to copy.

### Plugin boundary — keep the C ABI, generate the typed layer, kill the schemaless middle

**Keep verbatim:** the frozen C ABI with the CLAP-style `get_interface(id, version)` capability door (`xi_abi.h:486-517`) and the segregated `xi.imaging/doc/emit/log/preview@1` interfaces. This is the right way to evolve host capability without breaking layout, and the freeze-guard that asserts every carved interface pointer equals its struct-field twin (`ImagePool::door_matches_fields`, `xi_image_pool.hpp:698-740`) is a real projection-can't-drift gate — the law in miniature. Keep the min-compat load gate (`XI_ABI_MIN_COMPAT`) and the per-export in-plugin `try/catch` that makes the boundary noexcept-in-practice across CRTs (`XI_PLUGIN_IMPL`, `xi_abi.hpp:748-765`).

**Redesign:** the `_keys.h` + `_io.h` builder/extractor pattern is the right *idea* implemented the wrong *way* — hand-written three times (keys, builder, extractor) and, per Finding 3, already drifting. Make the schema the source and generate the rest:

- An operator declares one **`operator.toml`** (or a small DSL): its `produces`/`consumes` keys with types, its params, its exchange commands, its schema version.
- Codegen emits, from that one file: (a) the typed C++ `Input`/`Output` views over `xi::Frame` — *complete*, so a contour accessor can't go missing; (b) the wire `record_schema` the ABI already reserves an export slot for (`xi_plugin_record_schema_fn`, `xi_abi.h:731`); (c) the `plugin.json` manifest block; (d) the reference doc stub. One declaration, four projections, gated for drift.
- This turns the current 130 lines of hand-scaffolding that 7 of 9 plugins skip into zero lines an author writes and one file they edit. The schemaless `Record` disappears from the authoring surface entirely; it survives only inside `expose`, which is *legitimately* schemaless (arbitrary UI payloads) and correctly opts out (`expose` uses `xex1_encode.hpp` as its single-source codec — keep that verbatim).

**Delete:** the notion that a plugin's contract is "declarative but purely advisory." If a plugin declares it consumes `gray:image`, the host validates that at wire/load time and refuses a mismatched wiring — the schema *bites*, it isn't decoration.

### Script experience — keep C++ and hot-reload, demote the JIT to a dev affordance

**Keep verbatim:** C++-as-script. The one-file `XI_INSPECT_ENTRY(t, frame)` with an *explicit* trigger view passed by parameter (`xi_use.hpp:674-681`) is genuinely good — it killed the ambient-thread_local footgun (a worker calling `current_trigger()` and silently getting nothing) by making the trigger a self-contained, thread-safe, by-value-capturable object (`Trigger(const xi_trigger_view*)`, `xi_use.hpp:174-197`; `TriggerSnapshot`, `:407-472`). Keep hot-reload with state serialization across DLL unload.

**Redesign the toolchain assumption.** Requiring `cl.exe` + a full OpenCV at *runtime* on the factory PC (README "MSVC C++ toolchain and OpenCV must be present at runtime") to JIT-compile the inspection is a heavy production dependency for what is, in production, a fixed artifact. Invert the default: **production ships an AOT bundle** (the script + its plugins precompiled, no toolchain on the line); **JIT-to-DLL is the developer inner loop only.** The current runner already proves the AOT path works headless — make it first-class and make the toolchain optional off the dev box.

### Protocol & contract — one IDL, generated clients, a gate that cannot skip

The current state is three hand-rolled protocol implementations (`vscode-extension/src/wsClient.ts`, `ui-components/src/ws-client.mjs`, `tools/xinsp2_py/.../client.py`), request/response id-correlation reimplemented *four* times (the VS Code extension re-adds it a fourth time in `extension.ts` because `WsClient` doesn't provide it), a typed `protocol.ts` used only by tests while production inlines its own parsing, and three descriptions of the wire (schemas, generated types, `xi_protocol.hpp`) that no gate triangulates. This is the law violated at fleet scale.

**Redesign:** one **protocol IDL** owns the wire. Generate the envelope parser + id-correlation + typed message structs for each language (TS, Python, and the C++ `xi_protocol.hpp`) from it. The clients keep their *policy* differences (reconnect, auth mode, single-client-busy handling) but share the *generated* transport core. `xi_protocol.hpp` stops being a third hand-written description and becomes a generated projection like the rest.

**Keep** the `baseline_gate.py` idea (a committed schema-shape snapshot, additive-vs-breaking classification) — it is pure-stdlib and *does* run on CI. But close two holes: (a) tie `baseline.protocol_version`, the live wire `abi` stamp, and `compat-matrix.json`'s `ws_protocol_abi` together in one triangulating check (today they are pairwise ungated, `baseline_gate.py:28-30`); (b) make the schema-conformance validator **unable to skip** — either vendor a pinned `jsonschema` as a hard build dependency or, better, write the subset validator in pure stdlib so the "library absent → SKIP" branch (`validate.py:35-39`) cannot exist. A gate that can silently pass is not a gate.

### State / health / lifecycle — elevate the health registry's principle to law

**Keep verbatim:** the `HealthRegistry` (`xi_health.hpp`) and its derive-don't-duplicate discipline — this is the model for the whole core. Keep the verdict code-band convention (`xi_result.hpp:7-16`) and the honest emission that a completed-but-no-`result()` run is `XI_SYS_NO_VERDICT` and a caught crash is `XI_SYS_CRASHED`, not a silent `0/NA` (`service_inspect.cpp:259-289`). Keep the `InflightRuns` Dekker handshake (`xi_inflight_runs.hpp:55-82`) and the single-source teardown ordering in `controlled_shutdown_teardown_` (`service_dispatch.cpp:426-476`), including the hard-`_Exit` on a wedged drain rather than a UAF'ing clean teardown (`:448-462`) — that is exactly the right trade and it was clearly earned.

**Redesign:** make the ownership map explicit and total. Today it is *mostly* clean but the instance-state map only landed in PluginManager after it drifted as a side map (the comment at `service_dispatch.cpp:616-622` documents the scar). xInsp3 should ship a written **ownership ledger**: one table naming, for every runtime fact, its sole owner and its derived readers — and a test that fails if any fact grows a second writer. The bug class is structural; the defense should be structural.

### Client fleet — fewer clients, one generated core

**Keep:** the browser fleet's convergence — `hmi` reuses `ui-components`'s `XiClient` rather than reimplementing it, and the single-client-busy contract (`BUSY_CLOSE_CODE = 4003`) has one home shared by the shim and the hmi proxy. That is the law obeyed; extend it. Fold the VS Code extension and the Python client onto the *generated* transport core (above), leaving each only its policy layer.

**Redesign the single-WS-client topology, carefully.** The current "one control client, everyone else rejected with 4003" is a defensible simplification but it forces a choice between the operator HMI and the developer extension. Split the surface: **one control channel** (mutating commands — compile, set_def, start/stop) admitted singly, and a **fan-out read-only event channel** (run_result, health_changed, expose frames) that any number of observers may attach to. Production wants the operator panel *and* a remote monitor watching the same line; the data plane already supports it (events are broadcast), only the socket policy forbids it.

### Testing & gates — real teeth, no silent skips

**Keep:** the `gate.py` ↔ `ci.yml` single-source stage wiring (CI runs `gate.py --only …` / `--skip …`, so stage membership can't drift), the pure-stdlib checks that genuinely run on CI (`doc_coverage`, `retired_terms`, `baseline_gate`), and the self-guards that fail if an extractor silently matches zero (`check_doc_coverage.py`'s "< 40 commands ⇒ fail", `compat_manifest`'s ≥40 floor).

**Redesign the philosophy to one rule:** *a check that can pass without running is not a check.* Purge every `SKIP → exit 0` branch (`validate.py:35-39` is the flagship offender; the `flaky:`-classed QA quarantine that can never fail the suite is another). Make `doc_coverage`'s "documented" mean "has a real reference section," not "the token appears in any `.md`." Make the codegen (typed I/O, protocol, manifest, schema) **regenerate-and-diff** in CI so a stale generated artifact fails — the current `gen_types.py` is wired to nothing and its one test self-disarms when the generated file is missing.

### Runtime topology — keep the split, keep in-process plugins

**Keep verbatim:** the FE-supervisor / BE-compute split with rate-limited respawn and crash-history; all plugins in-process for zero-copy, accepting that a hard plugin crash takes the BE down and the FE respawns it; line-safety as a comm plugin's own crash-watching sidecar rather than a core concern (`xi_abi.h:382-385`). This is the correct trade for a speed-first single-machine framework and I would not reintroduce process isolation — SHM's removal (2026-05) was right. Keep the SEH → C++ translation wrapping every script/plugin call site (`service_inspect.cpp:142-217`) and the per-inspect watchdog with a cooperative-cancel ticket that doesn't poison a fresh frame started during the grace window (`:150-157`).

---

## KEEP / REDESIGN / DELETE

| Verdict | Item | Mechanism / replacement |
|---|---|---|
| **KEEP** | ImagePool | slot⊕generation ABA defense, Treiber free-list, WalkGuard deferred reclaim (`xi_image_pool.hpp:101-111, 780-888`) |
| **KEEP** | C ABI + capability door | `get_interface(id,ver)`, carved frozen interfaces, `door_matches_fields` freeze-guard (`xi_abi.h:486-517`, `xi_image_pool.hpp:698-740`) |
| **KEEP** | Explicit-trigger entry | `XI_INSPECT_ENTRY`, self-contained `Trigger`/`TriggerSnapshot` (`xi_use.hpp:174-197, 407-472`) |
| **KEEP** | Health registry | derive-don't-duplicate; store only the 3 unowned facts (`xi_health.hpp:9-17`) |
| **KEEP** | Teardown + inflight discipline | Dekker handshake, single-source ordering, hard-`_Exit` on wedged drain (`xi_inflight_runs.hpp`, `service_dispatch.cpp:426-476`) |
| **KEEP** | Verdict honesty | `XI_SYS_NO_VERDICT` / `XI_SYS_CRASHED` band, host as trust boundary (`xi_result.hpp`, `service_inspect.cpp:259-289`) |
| **KEEP** | FE/BE split, in-process plugins, SEH+watchdog | supervisor respawn; sidecar line-safety; per-inspect cancel ticket |
| **REDESIGN** | Per-frame data plane | replace schemaless yyjson `Record` with typed arena `xi::Frame`; JSON only at wire/config/disk |
| **REDESIGN** | Plugin contract | codegen typed I/O + wire schema + manifest + docs from one `operator.toml`; contract *bites* at load time |
| **REDESIGN** | Script toolchain | AOT bundle is the production default; JIT is a dev-box affordance |
| **REDESIGN** | Protocol | one IDL → generated transport core per language; triangulate the three version stamps |
| **REDESIGN** | Client topology | one singleton control channel + a fan-out read-only observer channel |
| **REDESIGN** | Gate philosophy | no `SKIP→exit 0`; regenerate-and-diff all codegen; "documented" means a real doc section |
| **DELETE** | γ / γ-4 doc-by-pointer machinery | `DocBox`, `share_out`/`adopt_shared` reserved-ref dance, cross-ABI COW + JSON-fallback ref balancing (`xi_record.hpp:94-252`, three-site balance in `xi_use.hpp`/`xi_abi.hpp`/`runner_main.cpp`) — obviated by the value-type `Frame` |
| **DELETE** | `data_output` no-op `save` | dead duplicate of `record_save` (`data_output.cpp:51-54`) |
| **DELETE** | dead `protocol/messages.md` pointer + unused `protocol.ts` in production | replaced by the generated protocol projection |

---

## The one thing

If I could bet the next generation on a single insight, it is this: **the core's real job is to own each truth exactly once and mechanically project it everywhere else — and the highest-leverage act is deleting the schemaless mutable Record from the per-frame boundary, because that one deletion is where the law pays off hardest.**

Every hardening scar in this codebase is a second representation drifting from a first: the instance-state map that had to be pulled into its owner, the capability door that needs a freeze-guard against the struct field, three WS clients and four id-correlators, three descriptions of the wire, an exemplar's typed `Output` that can't read its own contour. The health registry already discovered the cure and *named* it — "duplicating them here is the exact hand-synced-representation failure the architecture exists to kill" (`xi_health.hpp:14-17`). The per-frame `Record` is the same failure at the hottest, most-touched seam in the system: to make schemaless mutable JSON fast enough to cross a DLL boundary per frame, the design grew a refcounted document, copy-on-write, a cross-ABI enroll/adopt handshake with a reserved ref, and a JSON-fallback balance that must be re-derived correctly in three files — and the reward is a *deliberate leak on every crash* because a torn call is too intricate to unwind safely (Finding 4).

Replace it with a typed, arena-allocated value `Frame` and the entire apparatus vanishes: no DocBox, no COW, no cross-CRT `free`, no reserved-ref accounting, no leak-over-UAF trade. The typed `_io` views the plugins already hand-write (and get wrong) become *generated projections* of one operator schema. The wire schema, the manifest, and the docs become projections of the same schema. And the gate that guards those projections is rewritten so it **cannot silently skip**.

Own once. Project everywhere. Gate the projection. Start by deleting the Record.
