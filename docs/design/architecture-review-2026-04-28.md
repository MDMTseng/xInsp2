# xInsp2 architecture review — 2026-04-28

A frank read of where the design pays off, where it's paying interest, and what to do next. Aimed at the project owner. Not a sales pitch.

---

## 1. What this thing is actually optimising for

Read across `README.md`, `docs/architecture.md`, `docs/status.md` Decision log, and the actual code shape, the optimisation function is unambiguous:

- **Authoring throughput for one developer at one machine.** The whole flow is "edit a `.cpp`, save, hot-reload, see `VAR()` in a panel". Decision log: "VS Code is the IDE", "Script-first authoring UX. No graph editor", "Single client at a time. Multi-client deliberately deferred", "C++ compile path via MSVC `cl.exe`. No Cling / ClangREPL". (`docs/status.md:158-175`)
- **Decade-stable plugin ecosystem.** Plain C ABI for plugins (`backend/include/xi/xi_abi.h`), no C++ types across the boundary, append-only struct evolution, per-plugin `cert.json` baseline. The cost is real (everything is `xi_record` marshalling) but the prize is plugins compiled in 2024 still loading in 2030.
- **Crash survivability without a hypervisor.** `_set_se_translator` + `try/catch` at every call site (`backend/include/xi/xi_seh.hpp`, `service_main.cpp:573`), plus `SetUnhandledExceptionFilter` writing minidump + JSON sidecar (`service_main.cpp:2128-2210`). Crashes are recoverable when possible and forensically diagnosable when not.
- **Headless production runner that's not a fork of the dev backend** — `xinsp-runner.exe` shares all of `backend/include/xi/`, just no WS. (`backend/src/runner_main.cpp` vs `service_main.cpp`.) Same code, less surface.

What it deliberately is **not** trying to be:

- A node-graph DAG framework. The script *is* the graph.
- A multi-tenant inspection server. WS is single-client by spec (`xi_ws_server.hpp:3-8`).
- Cross-platform today. Windows-first; SEH, `cl.exe`, named pipes, `CreateFileMapping` are all intentional Windows leans (`docs/testing.md:165-167`).
- Dependency-heavy. Only cJSON + stb_image_write are vendored; OpenCV/IPP/turbojpeg are optional behind `XINSP2_HAS_*`.

This is internally consistent. The friction comes from places where features grew faster than the constraints they violate were re-examined.

---

## 2. Where the architecture pays off

1. **Plugin ABI stability is real, not aspirational.** `backend/include/xi/xi_abi.h:1-300` defines all six plugin entry points and the host_api in plain C. The C++ wrappers (`xi_abi.hpp`) are header-only — the moment a plugin compiles, it's bound to *no* xInsp2 symbol other than what's in `xi_abi.h`. Worked example: `examples/circle_counting/plugins/` plugins build out-of-tree against just the SDK and load against the master backend without recertification.

2. **Sharded ImagePool genuinely scales.** `backend/include/xi/xi_image_pool.hpp` — 16 shards keyed on `handle & 0xF`, each with `shared_mutex`, 64-bit internal counter for ABA prevention, delete-outside-lock on release. `test_image_pool` exercises the concurrent path. The `image_create` / `image_data` / `image_release` triplet is what allows plugins compiled in any language ABI to share pixel buffers safely across DLLs.

3. **SEH + crash filter + JSON sidecar combo.** `service_main.cpp:2128-2210` writes both a minidump and a small JSON next to it with `last_cmd`, `last_script`, `last_instance`, `last_plugin`, exception code, and `blame_module(addr)`. The next backend startup reads these and `cmd:crash_reports` surfaces them to the extension. This is the kind of plumbing most teams *talk* about and never ship. The `on_terminate` handler at `service_main.cpp:2222-2247` re-raises with a distinct code (`0xE0000002`) so silent terminate paths still produce a dump. Substantial work.

4. **Hot-reload with state survives because the contract is small.** `xi::state()` (`xi_state.hpp`) round-trips a JSON string through `xi_script_get_state` / `xi_script_set_state`; `xi::Param<T>` values are replayed; `xi::use("name")` re-resolves through the backend's `InstanceRegistry`. Three primitives, no magic, and they cover the whole "edit→save→keep going" UX.

5. **TriggerBus is opt-in and back-compat.** `xi_trigger_bridge.hpp` auto-bridges legacy `ImageSource::push()` plugins into the bus as `emit_trigger(name, fresh_tid, …)`. New multi-camera plugins use 128-bit tids; old plugins keep working. The Decision log calls this out and the bridge code makes it true.

6. **Manifest pass-through is a quietly excellent piece of design.** `xi_plugin_manager.hpp:360, 1387` extracts the verbatim `manifest` JSON block from `plugin.json` and forwards it through `cmd:list_plugins` (`xi_plugin_manager.hpp:1324-1325`). UI clients — including AI agents driving `cmd:list_plugins` — can introspect a plugin's params, ranges, and defaults without grepping source. This is the right abstraction at the right place.

7. **Process-isolation mesh actually works on its own terms.** `xinsp-worker.exe` + `xinsp-script-runner.exe` + `xi_shm.hpp` (512 MB CreateFileMapping with cross-process atomic refcount) + `Session` bidirectional RPC for `use_*` callbacks. 9 spike tests green (`test_xi_shm`, `test_worker`, `test_worker_respawn`, `test_worker_timeout`, `test_script_runner`, `test_script_process_adapter`, `test_script_runner_respawn`, `test_isolated_instance`, `test_shm_ipc_edges`). The respawn rate-limit (3/60s) and `CancelIoEx` watchdog in `ProcessInstanceAdapter::raw_call_locked_` (`xi_process_instance.hpp:244-293`) are the two bits that keep "isolation works" from becoming "isolation deadlocks".

8. **Headless runner is genuinely a runner, not a stripped backend.** `runner_main.cpp` uses the same `xi_plugin_manager`, `xi_script_compiler`, `xi_script_loader` — no parallel fork. The Decision log "Headless backend. Any WS client can drive it" is honoured by the file structure, not just the README.

---

## 3. Where the architecture is paying interest

These are real, with file/line cites. Stop-gaps are flagged as such.

### 3.1 Process isolation: shipped as opt-in, several gaps blocking default-on

The `shm-process-isolation` PR is merged but the surface is incomplete. From `docs/reference/ipc-shm.md:1-11` and `docs/status.md:107-138`:

- **`ProcessInstanceAdapter::process_via_rpc` only carries one input image and one output image.** `xi_process_instance.hpp:172-204`:
  ```
  uint64_t in_h = (in && in->image_count > 0 && in->images) ? in->images[0].handle : 0ull;
  ```
  and a single `out_image_` / `out_key_` member. **Multi-image plugins are broken in isolated mode.** No comment in the file flags this as TODO; the docstring says "Single-image output is the common case" — true, but plugins that emit `mask` + `overlay` + `debug_view` in one Record silently lose two of three handles. This is the largest correctness hole in the merge.
- **Plugin-side handle storage in JSON not handled.** A plugin that stashes an image handle in its `set_def`/`get_def` JSON across calls is moving an integer handle that's only valid in *that worker's* process. The `saved_def_` replay on respawn (`xi_process_instance.hpp:383-391`) does not rewrite handles. Probably fine for current shipped plugins (none do this) but a tripwire for third-party plugin authors.
- **No fail-loud policy when a worker goes dead.** Once `dead_ = true` and the 3/60s respawn cap is hit, every method returns safe defaults silently: `get_def → "{}"`, `set_def → false`, `exchange → "{}"`, `process_via_rpc → false` with `err` set but no surface to user. `docs/reference/ipc-shm.md:213-216` flags this as an open question. The script keeps iterating against a no-op detector. Real factories will not love this default.
- **Heap-pool → SHM auto-conversion only on output.** `worker_main.cpp` does the auto-copy for plugin-side `xi::Image{...}` outputs (per status.md), but the **input** path still passes a single handle (above). Plugins that allocate with `image_create` and the framework wraps them — fine; plugins that allocate with `shm_create_image` directly — also fine; mixed cases on the input side at multi-image granularity — untested.
- **Script-side default-on isn't wired.** `cmd:run` still uses the in-proc path; `cmd:script_isolated_run` is a separate command. `docs/reference/ipc-shm.md:217-221` correctly identifies this as ~50 lines but interacting with history / triggers / breakpoints. Real cost is the wiring of `emit_vars_and_previews` against SHM-resolved gids, not the spawn.

### 3.2 The `frame_path` phantom argument

Confirmed still present and still ignored.

- Wire format carries it: `protocol/fixtures/cmd_run.json`, `vscode-extension/test/protocol.test.mjs:22`, the Python client (`tools/xinsp2_py/xinsp2/client.py:149-160`) explicitly takes `frame_path=`.
- Backend handler ignores it: `backend/src/service_main.cpp:1162` calls `run_one_inspection(srv, /*frame_hint=*/1, run_id)` — the `args_json` is parsed elsewhere but `frame_path` is never read by the run path. Grep confirms zero consumers in `service_main.cpp` other than the `frame_hint` integer.
- Already flagged externally: `examples/circle_counting/RESULTS.md:120-156` wrote 150+ lines about this confusion and proposed `xi::current_frame_path()` or `xi::imread()` as remedies.

This is API-surface debt that bites every new user who reads the protocol doc before the script doc. Either drop it from protocol/client/fixture or surface it via a thread-local accessor that the script can read. Don't leave it half there.

### 3.3 Ad-hoc JSON parsing in `xi_plugin_manager.hpp`

`extract_string` (`xi_plugin_manager.hpp:1391-1404`) and `detail_find_key` (`:1406-1434`) are bespoke string-scan parsers with predictable failure modes:

- `extract_string` finds the first `"key"` substring in the file — does **not** check that it's a key, not a value. A `plugin.json` containing `"description": "the name field"` will collide with `extract_string(json, "name")` if the description appears first. (For shipped `plugin.json` files this doesn't happen, but the code is brittle.)
- `detail_find_key`'s string-skip logic at `:1420` advances past escaped quotes by `if (*p == '\\') p++; p++;` but there is a missing bounds check before the second `p++` — a malformed manifest ending mid-string-escape can read past `end`. Probably benign on local files, not robust against an attacker-controlled `extra_plugin_dirs` payload.
- The entire manager already pulls in cJSON via the rest of the codebase. There's no upside to hand-rolling. This is the kind of code that ships and never gets touched until it crashes someone else's `plugin.json`.

### 3.4 Single-client constraint is structural

`xi_ws_server.hpp:3-8` says "minimal single-client WebSocket server". This isn't just an implementation detail:

- The `g_continuous` flag (`service_main.cpp:1169`), the `g_run_mu` mutex serialising runs (`:1161`), the `crash_set(g_crash_ctx.last_cmd, ...)` global crash context (`:1157, :2188-2193`), the singleton `TriggerBus::instance()` (`:1194`) — all assume one driving client.
- S6 (multi-client broadcast) is "deliberately deferred" (`docs/status.md:185`). Fine. But adding it later is *not* a server swap; it's untangling globals from session state across half the file.

### 3.5 `VAR` macro redeclaration trap

`docs/guides/writing-a-script.md:166-176` documents it ("`VAR(name, ...)` declares a local") and `examples/circle_counting/RESULTS.md:165-179` confirms it cost a real user a compile cycle. The fix is small — add a `VAR_TRACK(name, existing_var)` macro that doesn't redeclare — but it hasn't shipped, so the documentation patch is the only mitigation.

### 3.6 In-proc crash class still kills the backend on the default path

`docs/testing.md:158-162` is honest: "Plugin / script crashes still kill the backend on the default in-proc path." SEH translation handles AV / div0 / array overrun, but stack overflow, heap corruption, fastfail, and silent terminate paths in worker threads still take the process. The crashdump pipeline catches them post-mortem and the extension auto-respawns the backend, but a 200-frame production run with one bad inspection cycle still loses 199 frames of work.

### 3.7 Project-file write path is a string concatenation

`save_project_locked` (`xi_plugin_manager.hpp:1436-1450`) builds `project.json` by string concat of `project_.name`, `script_path`, etc. No JSON escaping. A project named `Acme "Edge"` will produce invalid JSON. Crash-safe atomic write (`xi_atomic_io.hpp`, ✅ in status.md) does not save you from invalid content.

### 3.8 Plugin instance/script DLL versioning is a workaround, not a model

`stem_vN.dll` naming (`docs/status.md:62`) exists because Windows holds a load lock on the previous DLL until unload completes. Hot-reload works *most* of the time; pathological cases (a worker thread inside the plugin that hasn't joined yet) leave the file locked and N grows monotonically. It's pragmatic but creates orphan files in the project tree.

---

## 4. Architectural debt by severity

Ranked by impact × cost-to-fix. "If 1 week / 1 month / 3 months" framing.

| # | Issue | Impact | Cost | Action |
|---|---|---|---|---|
| 1 | `frame_path` phantom in protocol | Med (every new user trips on it) | XS (1 day) | **1-week**: either delete from fixture/client/protocol or wire `xi::current_request().frame_path()` thread-local |
| 2 | `ProcessInstanceAdapter` single-image input/output | High (silently drops outputs in isolation mode; blocks default-on) | M (1 week) | **1-week**: extend `process_via_rpc` to loop over `images[]` on both sides; tests for 2 and 3 image cases |
| 3 | Fail-loud when worker dead | High (silent no-op in production) | S (2 days) | **1-week**: emit `cmd:isolation_dead` event to client; surface as toast in extension |
| 4 | `extract_string` / `detail_find_key` ad-hoc parser | Med (brittle, third-party plugin JSON could trigger) | S (1 day) | **1-week**: replace both with cJSON calls (already linked) |
| 5 | `save_project_locked` no JSON escaping | Med (one quote in a project name = corrupt file) | XS (½ day) | **1-week**: route through `xi::Json` builder |
| 6 | `VAR` redeclaration trap | Low (cost: 1 compile cycle per new user) | XS (½ day) | **1-week**: add `VAR_TRACK(name, var)` macro + doc update |
| 7 | Plugin-side handle in JSON not rewritten on respawn | Low today, High if any third-party plugin does it | M | **1-month**: document as unsupported OR add a handle-key list to `set_def` envelope |
| 8 | Script-side `cmd:run` not isolated by default | High (the headline limitation users hit) | L (2 weeks) | **1-month**: wire `ScriptProcessAdapter` into `cmd:run` path with snapshot-vars routing back over Session RPC |
| 9 | Single-client globals (`g_run_mu`, `g_continuous`, `g_crash_ctx`) | Med (blocks S6 multi-client) | L | **3-month**: collapse globals into a `BackendSession` object; multi-client becomes adding a second one |
| 10 | Stack overflow / heap corruption still kill process | Med (motivated process-isolation in the first place) | already mitigated by isolation | **3-month**: once #8 lands, default-on isolation closes this for scripts; for plugins it's already opt-in |

---

## 5. What's surprisingly well-designed

- **Crash forensics pipeline.** The combination of `_set_se_translator` (per-thread, recoverable case) + `SetUnhandledExceptionFilter` (process-fatal case) + `set_terminate` (silent C++ exception in detached thread case, `service_main.cpp:2222-2247`) + `AddVectoredExceptionHandler` (early-warning logging, `:2259-2283`) + minidump + JSON sidecar with `last_cmd` / `last_script` / `last_plugin` / `blame_module` is *belt-and-three-suspenders* coverage. Most production C++ services have one of these, claim they have two. xInsp2 has all four and they're wired to a notification UX.

- **Manifest pass-through.** `xi_plugin_manager.hpp:1324, 1387` reads the manifest block as opaque JSON and re-emits it on `cmd:list_plugins`. This is the one piece of plumbing that lets an AI agent (or any UI client) discover plugin params without grepping source. Cost: ~10 lines. Value: every future automation sits on top of it. The recent autonomous-inspection work depends on this and would have been much uglier without it.

- **Worker-side heap-pool→SHM auto-copy.** `worker_main.cpp` transparently moves plugin outputs allocated with `xi::Image{...}` into the SHM region so the plugin author doesn't have to know about isolation. The right kind of leak-from-the-host: the plugin writes naive code, the framework hides the IPC boundary.

- **Per-plugin baseline cert.** Eight C-ABI safety checks run on first load, cached in `cert.json`. A failed baseline refuses instantiation. This single mechanism prevents the "`xi_plugin_create` returns garbage and we segfault on every subsequent call" class of bug from reaching production. Few projects this size bother.

- **Plugin / script DLL re-versioning (`stem_vN.dll`).** Ugly, but the alternative ("close VS Code to unlock the DLL") is a UX killer. Pragmatism wins here.

---

## 6. Concrete next-steps recommendation

The owner is on (a) AI-driven autonomous inspection development and (b) process-isolation default-on. Here is the highest-compounding order:

### A. **Land "default-on isolation" by way of fail-loud + multi-image first.** (2-3 weeks)

The blocker isn't spawning workers — that's done. The blockers are:
1. Multi-image input/output in `ProcessInstanceAdapter::process_via_rpc` (debt item #2).
2. Surface "worker dead" to the UI as a real event, not silent `{}` returns (#3).
3. Document the JSON-handle case as unsupported and add a baseline check that fails plugins which try (#7).

Once those three are in, flip the default for new instances created via UI. Existing `instance.json` without `"isolation"` keeps in-proc for back-compat. **Why this first**: every other item below is more pleasant when a script crash doesn't take the backend with it. AI-driven exploration in particular generates lots of bad scripts.

### B. **Make `frame_path` work, both directions.** (1 week)

Either delete it (fixture, client, doc) or wire `xi::current_request()` as a thread-local string the script can read via `xi::Json` parse. Combine with shipping a `xi::imread(path)` (stb_image.h is public domain, ~2k lines, already vendored its sibling). **Why this**: AI-driven inspection workflows are exactly the case where "give the script a frame path and a script and ask for an answer" is the obvious shape. Today the agent has to either (a) pre-convert PNGs to `.raw` and use a frame-index `Param` (`circle_counting/RESULTS.md:79-81`) or (b) build a custom plugin per task. Both are ceremony the agent shouldn't pay. Removing this friction compounds with every autonomous run.

### C. **Replace ad-hoc JSON helpers with cJSON, and route `save_project_locked` through `xi::Json`.** (3 days)

Items #4 and #5 in the debt table. Cheap, eliminates a class of corruption bugs, and removes a blast radius that grows as third-party plugins ship arbitrary `plugin.json` content. Do this before opening the plugin loader to anything you don't fully control.

These three deliver: (a) a crash-resilient default that lets autonomous agents fail fast and recover, (b) a frame-input affordance that matches how AI agents already think, (c) hardening of the trust boundary now expanding to third-party content. Each one is independently shippable; together they reset the "obvious next problem" for at least a quarter.

Things to **not** do next: multi-client (S6), Linux port, a node-graph editor. They'd burn weeks against the optimisation function this codebase is actually pursuing.
