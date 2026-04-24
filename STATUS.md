# xInsp2 — Status & Forward Plan

`NewDeal.md` is the stable vision; `DEV_PLAN.md` is the milestone skeleton;
`FRAMEWORK.md` is the authoritative technical reference; this file is the
weekly pulse.

Last full sync: 2026-04-24. 62 commits on `master`; Phase 3 (TriggerBus +
multi-camera + recording) is in flight, uncommitted.

## Where we are

**M0–M10 complete.** The original NewDeal scope shipped:

- **M0–M1** — core C++ headers + protocol schema, C++/TS parity.
- **M2–M3** — backend service, `run`, `vars` emission, `set_param`.
- **M4** — JPEG preview path (`xi_jpeg.hpp` dispatches IPP / OpenCV / stb).
- **M5** — `compile_and_load` with MSVC + multi-file + versioned DLL naming
  (`stem_vN.dll`) to survive Windows DLL locks during hot-reload.
- **M6** — `Instance<T>` / `Param<T>` registries + `project.json` +
  per-instance folders (`<project>/instances/<name>/`).
- **M7–M8** — VS Code extension: TreeView, Viewer webview, CodeLens on both
  declarations **and usages**, plugin UI webviews with IIFE shim.
- **M9** — real inspection routines shipped (`defect_detection.cpp`,
  `use_demo.cpp`, `user_with_instance.cpp`, `plugin_handle_demo.cpp`).
- **M10** — old Electron/graph code not present in this tree; xInsp2 is
  the only shipped frontend.

**Beyond the original scope:**

- **Stable C ABI for plugins** (`xi_abi.h` / `xi_abi.hpp` / `XI_PLUGIN_IMPL`).
  No C++ types cross the boundary; safe across MSVC versions.
- **Sharded refcounted ImagePool** (16 shards, `shared_mutex`, 64-bit
  internal counter, ABA-proof handles).
- **SEH crash isolation** via `_set_se_translator` on all script / plugin
  call sites. Null deref, div/0, array overrun, C++ throw all recoverable.
- **`xi::state()`** — persistent cross-frame, cross-reload state via
  serialize-before-unload / restore-after-load thunks.
- **`xi::use("name")`** — proxy to backend-managed instances. Survives
  script hot-reload. Mutex-protected proxy cache.
- **`Record`** — cJSON-backed universal data container; chained
  `operator[]`, path expressions (`rec["a.b[3].c"]`), safe defaults.
- **`xi::Json`** — light JSON builder used across plugins + host.
- **Cert system** — `xi_cert.hpp` per-plugin cert files (`cert.json`).
- **Plugin SDK** — `sdk/` has scaffold (`scaffold.mjs`, `create_plugin.sh`),
  `sdk/cmake/xinsp2_plugin.cmake`, `sdk/template/` with `tests/`, and
  `sdk/testing/` helpers for E2E.
- **7 shipped plugins** — `mock_camera`, `blob_analysis`, `data_output`,
  `json_source`, `record_save`, `threshold_op`, `synced_stereo`.

**Test surface (per TEST_PLAN.md):**

- 43 C++ unit tests (`test_xi_core`, `test_protocol`, `test_record`,
  `test_ops`, `test_image_pool`) — all green.
- 22 Node integration tests (18 pass / 3 skip / 1 pending) covering WS
  basic, run, crash recovery, compile fail, SEH, plugins, project open,
  JPEG preview, param set, hot-reload + state, process_instance.
- VS Code E2E harness with screenshotted pipeline runs
  (`runPipeline.mjs`, `runE2E.mjs`).
- New E2E runners (uncommitted): `runMulticam.mjs`, `runRecordReplay.mjs`,
  `runUxStates.mjs`.

**Code budget:** ~8.3 kLOC in `backend/` (headers + service_main +
stb_impl). Original NewDeal target was 3 kLOC — exceeded because M6+ and
the whole C ABI / plugin system / SEH layer ended up inside xInsp2 rather
than reused from the old tree.

## Goal vs current state (NewDeal success criteria)

| Criterion                                                      | State |
|----------------------------------------------------------------|-------|
| Sequential `xi::*` script with live variable inspection        | ✅ ships |
| Slider tuning → live preview, no recompile                     | ✅ `set_param` + live preview webview |
| Parallel ops via `async_*`                                     | ✅ `Future<T>` with consumed guard |
| Breakpoints in `inspection.cpp` hit                            | ✅ cpptools-attach documented |
| Remote browser client can run the script                       | ✅ protocol is WS-only; any client works |
| <3 kLOC new code                                               | ❌ ~8.3 kLOC — scope grew (worth it) |

## Hardening status (was BugAudit.md)

All **4 CRITICAL** and **8 HIGH** bugs are fixed. The `BugAudit.md` status
table was stale when you read it — the source of truth is now FRAMEWORK.md
"Production Hardening Status" plus the commit log:

| Bug   | Fix commit                                                       |
|-------|------------------------------------------------------------------|
| A4-1  | `8f8b3d2` reject shell metacharacters in compile paths           |
| A2-6  | `ab919b7` `xi_image_handle` → `uint64_t`                         |
| A3-1  | `de15479` stop worker thread before DLL reload                   |
| A3-7  | `de15479` stop worker thread before shutdown                     |
| A1-1, A1-6, A1-8, A3-2, A3-3, A2-2, A2-5, A3-5 | `f16cf6a` 8-HIGH batch |

Remaining MEDIUM / LOW items (tracked here now that `BugAudit.md` is
retired — coverage status lives in `TestAudit.md`):

| ID | Sev | File | Issue |
|----|-----|------|-------|
| ~~A1-7~~ | ✅ MED | `xi_var.hpp` | `VarTraits<const char*>` / `char*` now copy into std::string (`25cdfd7`) |
| ~~A1-2~~ | ✅ MED | `xi_async.hpp` | `tuple_cat` filters void results; non-void Futures typed as before (`158e03b`) |
| ~~A1-4~~ | ✅ MED | `xi_record.hpp` | `image_keys_json` now uses `append_json_escaped` (`6b5db85`) |
| ~~A2-1~~ | ✅ MED | `xi_abi.hpp` | `(host, handle)` ctor removed; use `from_handle` / `share_handle` (`25cdfd7`) |
| ~~A2-3~~ | ✅ MED | `xi_record.hpp` | Verified non-bug: `Record::image(const std::string&, ...)` keys are deep-copied by `std::map` |
| ~~A2-4~~ | ✅ MED | `xi_plugin_manager.hpp` | `~PluginManager` frees handles; `scan_plugins` preserves live ones (`6b5db85`) |
| ~~A3-4~~ | ✅ MED | `service_main.cpp` | `process_instance` now catches `std::exception` + `...` with handle cleanup (`158e03b`) |
| ~~A4-2~~ | ✅ MED | `xi_ws_server.hpp` | Fragmentation now reassembled (`0f6a0a6`) |
| ~~A4-3~~ | ✅ MED | `xi_ws_server.hpp` | kMaxFrame dropped to 16 MiB; rx_buf_ bounded (`0f6a0a6`) |
| ~~A4-5~~ | ✅ MED | `xi_script_support.hpp` | Snapshot thunk uses `esc()` on image keys (`6b5db85`) |
| ~~A1-3~~ | ✅ LOW | `xi_record.hpp` | Path key now std::string, no truncation |
| ~~A1-5~~ | ✅ LOW | `xi_record.hpp` | `[-1]` and non-digit array index rejected |
| ~~A1-9~~ | ✅ LOW | `xi_param.hpp` | `Param::as_json` escapes name |
| ~~A3-6~~ | ✅ LOW | `service_main.cpp` | Dead `g_trigger_cv` / `g_trigger_pending` removed |
| ~~A4-4~~ | ✅ LOW | `xi_script_loader.hpp` | Contract block added: only `inspect` required |
| ~~A4-6~~ | ✅ LOW | `xi_script_support.hpp` | Lifetime invariant documented |
| ~~A4-7~~ | ✅ LOW | `xi_protocol.hpp` | Trust boundary on `data_json` documented |

**BugAudit findings closed: 4 CRITICAL + 8 HIGH + 10 MEDIUM + 7 LOW = 29/29.**

## In flight — Phase 3: TriggerBus & multi-camera (uncommitted)

Architectural upgrade from "source pushes frame → inspect fires" to
"sources emit under a 128-bit trigger id → bus correlates → dispatch one
event". Enables hardware-triggered multi-camera capture without each
plugin reinventing sync.

**New / changed files (44 files, +2656 / −447 diff):**

| File                                   | Role                                        |
|----------------------------------------|---------------------------------------------|
| `xi_abi.h`                             | `xi_trigger_id`, `host->emit_trigger()`     |
| `xi_trigger_bus.hpp` (new, 335 loc)    | Bus + policies ANY / AllRequired / LeaderFollowers |
| `xi_trigger_bridge.hpp` (new, 44 loc)  | Bridge legacy `ImageSource` plugins into bus |
| `xi_trigger_recorder.hpp` (new, 312 loc) | Observer-mode recorder + replay via bus    |
| `xi_use.hpp` (+105)                    | `xi::Trigger` / `xi::current_trigger()`     |
| `xi_script_support.hpp` (+19)          | Script-side trigger thunks                  |
| `xi_plugin_manager.hpp` (+192)         | Persisted `trigger_policy`, rename / remove instance |
| `service_main.cpp` (+274)              | Bus hook, event queue, 10 new WS commands   |
| `plugins/synced_stereo/` (new)         | Reference plugin: paired frames under one tid |

**New WS commands:**
`rescan_plugins`, `close_project`, `recording_start`, `recording_stop`,
`recording_status`, `recording_replay`, `set_trigger_policy`,
`recertify_plugin`, `remove_instance`, `rename_instance`.

**VS Code extension:** `extension.ts` +776 loc, new `pluginTree.ts`,
`package.json` +198, UX-state E2E runner.

**Outstanding:**
- Confirm all three trigger policies have integration coverage
  (`runMulticam.mjs` exercises AllRequired implicitly; need dedicated
  LeaderFollowers + Any tests).
- ~~`runRecordReplay.mjs` green on Windows~~ — ✅ fixed in `60d19d0`
  (folder paths were emitted unescaped, breaking client JSON.parse).
- Doc the `manifest.json` + `.raw` on-disk recording schema in FRAMEWORK
  (already in §20 after the Phase 3 sync).

## Immediate next steps

1. **Land Phase 3** — finish the trigger-bus test pass, commit in one or
   two clean chunks (abi+bus+service / plugin-manager+sdk / vscode).
2. **Close MEDIUM bugs** from BugAudit (WS fragmentation A4-2,
   64 MiB DoS alloc A4-3, JSON key escaping A1-4 / A4-5, HostImage ctor
   trap A2-1). Each is <50 lines.
3. **TestAudit follow-through** — raise coverage on the 26 untested bugs
   and the 43% of commands with no regression.

## Stretch roadmap (S-milestones, unchanged from original NewDeal)

| # | Name                       | Enabled by                   | Status |
|---|----------------------------|------------------------------|--------|
| S1 | Live preview subscription  | `subscribe` / `unsubscribe`  | protocol present, not wired |
| S2 | Editor: auto-compile on save | VS Code file watcher       | ✅ shipping |
| S3 | `xi::breakpoint("label")`  | protocol `event` + `resume`  | not started |
| S4 | Timeline / history         | Keep N past ValueStores      | partially enabled by Recorder |
| S5 | Operator library catalog   | `ASYNC_WRAP` library         | `xi_ops.hpp` subset shipped |
| S6 | Multi-client broadcast     | Session id + client vector   | not started (single-client still) |
| S7 | Recipe variant / A-B       | Two `project.json` variants  | not started |
| S8 | Recording / replay         | `xi_trigger_recorder.hpp`    | ✅ shipping (Phase 3) |
| S9 | Remote backend mode        | `--host 0.0.0.0` + auth      | ✅ shipping (`b0622b4`) |
| S10 | Headless production runner | `xinsp-runner.exe`          | ✅ shipping (`62f96ed`) |

## Decision log (locked in)

- **No BPG protocol.** Everything over WS framing.
- **No N-API.** Backend is a standalone exe.
- **No graph editor.** Script-first UX.
- **C++ compile path via MSVC `cl.exe`**, versioned DLL naming for
  hot-reload (`inspection_v3.dll`). No Cling / ClangREPL.
- **Stable C ABI for plugins.** No C++ types cross plugin boundary.
- **Dependency-free host.** Only cJSON + stb_image_write vendored;
  OpenCV / IPP optional via `XINSP2_HAS_*`. No uWebSockets, no
  nlohmann/json — `xi_ws_server.hpp` and `xi_protocol.hpp` cover it.
- **VS Code is the IDE.** No in-house editor.
- **Headless backend.** UI is optional — any WS client can drive it.
- **Single client at a time.** Multi-client is S6, deliberately deferred.
- **Per-instance folders.** Each instance owns
  `<project>/instances/<name>/`; `host->instance_folder()` gives plugins
  their own on-disk scratch space.
- **Trigger bus is opt-in.** Legacy `ImageSource` plugins work unchanged
  via `attach_trigger_bridge`; new plugins call `host->emit_trigger`.
