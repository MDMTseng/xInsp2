# xInsp2 Audit Round 1 + Round 2 — Consolidated Findings (2026-05-10)

Six independent audit agents reviewed the codebase. Round 1 split by file area
(A: dispatch / trigger / script; B: project / instance lifecycle; C: IPC + worker).
Round 2 split by stance (D: adversarial fault-injection; E: soak / accumulation;
F: API contract).

This file is the consolidated reference for downstream test-plan design.

---

## 🔴 P0 — must fix before production deployment

### Concurrency / lifetime / use-after-free class (cross-cutting; A + B converged)

**P0-AB-1.** `cmd:unload_script` doesn't `stop_dispatch_pool_()` / drain in-flight inspects
before `unload_script(g_script)` calls `FreeLibrary`. Worker thread that snapshotted
`s = g_script` and dropped `g_script_mu` is mid-`s.inspect(frame_hint)` when DLL is
unmapped. `service_main.cpp:1534-1543`.

**P0-AB-2.** `cmd:run` detaches a thread that snapshots `s` and drops the lock; concurrent
`compile_and_load` `FreeLibrary`s without joining. `service_main.cpp:1676-1680`,
`:1378-1392`.

**P0-AB-3.** `cmd:open_project` / `close_project` / `recompile_project_plugin` /
`export_project_plugin` don't drain dispatcher pool before mutating instances /
FreeLibrary. The `compile_and_load` and `shutdown` handlers explicitly do this; project-
lifecycle handlers don't. `service_main.cpp:2384`, `:2422`, `:2462`, `:2425`.

**P0-AB-4.** `recompile_project_plugin` resets `ii.instance` while dispatcher / `cmd:exchange`
holds shared_ptr copies; subsequent `set_def_fn_`/`exchange_fn_` calls into freed
DLL after step-3 FreeLibrary. `xi_plugin_manager.hpp:511, 540, 614, 621`.

**P0-AB-5.** `cmd:exchange_instance` re-entrancy through host_api hops can race
`recompile_project_plugin` post-FreeLibrary. Same root cause as AB-4.

**Common fix shape**: every project-lifecycle / DLL-touching handler must
`stop_dispatch_pool_()` first, drain in-flight calls, then mutate / FreeLibrary, then
optionally re-arm. Mirror `compile_and_load`'s pattern (line 1273) consistently.

### IPC layer

**P0-C1.** `Pipe::accept_one` `WaitForSingleObject(INFINITE)` — host hangs forever if
worker crashes between CreateProcess and CreateFile. `xi_ipc.hpp:165-186`.

**P0-C2.** `seq_` (`uint32_t`) wraps to 0 → routed to `handle_async_frame_` instead of
fulfilling promise. ~50 days at 1 RPC/ms; ~828 days at 60 fps.
`xi_process_instance.hpp:322, 512`.

**P0-C3.** `start_reader_` resets `stopping_=false` / `reader_dead_=false` without
holding `inflight_mu_`. Latent unless burst parallelism lands.
`xi_process_instance.hpp:422-423`.

### Adversarial / wire format

**P0-D1.** `xi_ipc.hpp::Reader::str()` and `Reader::bytes()` read attacker-controlled
length with NO bounds check against `end_ - p_`. Only `take()` bounds-checks.
A worker (or backend) sending a frame whose embedded `n=UINT32_MAX-4` causes 4 GB
OOB read / vector alloc. Affects every Reader-based decode (process / exchange /
get_def / set_def / use_* / emit_trigger). `xi_ipc.hpp:455-464`.

**P0-D2.** `xi_shm.hpp::alloc_image` int32 overflow — `int64_t pixels = w*h*ch`,
truncated to int32 in payload_size. Plugin allocates a 50000×50000×4 image →
`pixels=1e10` truncates → buffer too small but plugin writes full size →
corrupts neighboring SHM blocks. Silent until corrupted bytes read.
`xi_shm.hpp:235-237`.

**P0-D3.** `open_project` auto-load-on-demand path skips `plugin_abi_compatible` check
AND skips FreeLibrary on factory-not-found. A stale future-ABI DLL loads, gets called
with new ABI signatures, corrupts memory. `xi_plugin_manager.hpp:1170-1180`.

**P0-D4.** `recompile_project_plugin` failure-recovery only handles `c_factory != null`;
old-ABI plugins lose all instances permanently on compile failure. `xi_plugin_manager.hpp:589-602`.

**P0-D5.** WS handshake `do_handshake` uses blocking `::recv` with no per-call timeout.
Slow-loris peer (1 byte/min) holds the entire single-client backend hostage.
`xi_ws_server.hpp:415-423`.

### Soak / accumulation

**P0-E1.** `ShmRegion` is bump-only, `release` decrements refcount but never moves
`bump_offset` back. Self-acknowledged as "production version would maintain a
free-list". 512 MB region exhausts in **~17 s at 30 fps × 1 MB/frame**, **~1.4 s at
60 fps × 6 MB/frame**. After exhaustion `shm_create_image` returns INVALID_HANDLE;
worker silently pushes heap handle through `RPC_EMIT_TRIGGER`; host gets `nullptr` from
`image_data` and pipeline output silently degrades. **No alarm, no log.** Production-
blocking for any `isolation:process` deployment.
`xi_shm.hpp:312-338, 257-262`; `worker_main.cpp:202`.

**P0-E2.** `script_build/` and per-plugin `build/` directories accumulate forever.
Each `compile_and_load` mints `<stem>_v<N>.dll/.lib/.obj/.log`. `s_version++` never wraps
or prunes. ~5 GB/day at typical dev cadence; **150 GB after 30 days**.
`xi_script_compiler.hpp:298-303, 307`.

---

## 🟠 P1 — real risk under specific conditions

### Concurrency (Round 1)

- **A-P1-1.** `overflow:"block"` mode — workers `pop_front` but never `notify_one()`
  after popping. Producer waiting on `g_ev_cv` with `queue.size() < depth` may stall
  forever. `service_main.cpp:882-894, 943-944`.
- **A-P1-2.** `cmd:start` consumes stale events from prior cmd:stop window
  (no g_continuous gate on push). Cross-run image leakage. `service_main.cpp:1697`.
- **A-P1-3.** `release_all_for(owner_id)` in `unload_script` force-deletes pool entries
  while inspect holds a `xi::Image` wrapper. Same root as P0-AB-1.

### Project lifecycle (Round 1)

- **B-P1-1/2.** `open_project` partial-failure leaves stale `InstanceFolderRegistry` /
  `InstanceRegistry` entries pointing at destroyed adapters. Bounded leak (next open
  clears) but `cmd:list_instances` / `cmd:set_instance_def` find dead adapters.
  `xi_plugin_manager.hpp:1186, 1311, 1329-1335`.
- **B-P1-4.** `recompile_project_plugin` early-return paths reset `ii.instance` to null
  but never re-instantiate. Subsequent calls silently no-op. `xi_plugin_manager.hpp:560-563, 608, 622-624, 628-633, 643-646`.
- **B-P1-7.** `process_via_rpc` reads `out_keys_/out_images_/out_json_` outside `mu_`.
  Burst parallelism (task #71) will crash this. `xi_process_instance.hpp:248-269`.

### IPC (Round 1)

- **C-P1-1.** SHM region — no version handshake, no name versioning. Host/worker built
  from different commits silently corrupt. `worker_main.cpp:144-158`, `xi_shm.hpp:140-200`.
- **C-P1-2.** `recv_frame` partial-body delivery: header says 16 MB, peer sends 1 KB
  then crashes. With no in-flight RPC driving the timeout, reader thread blocks indefinitely.
  Cross-process emit_trigger idle period leaves no defense. `xi_ipc.hpp:308-321, 227-243`.
- **C-P1-4.** Respawn cap counts CreateProcess failures. 3 transient-OOM CreateProcess fails
  burn the whole 60 s budget. `xi_process_instance.hpp:621-727`.
- **C-P1-5.** `try_respawn_locked_` SET_DEF restore failure → `dead_=false` left set. Adapter
  reports alive but plugin runs default state — **silent wrong outputs** in production.
  Logged via `fprintf` to stderr only. `xi_process_instance.hpp:711-721`.
- **C-P1-6.** Worker `emit_trigger` releases SHM image before host's `TriggerBus::emit`
  addrefs. Slot may be recycled before host claims. `worker_main.cpp:230-243`.
- **C-P1-7.** Per-frame-type 16 MB payload cap is global. Hostile worker can send 16 MB
  error-message in `RPC_TYPE_ERROR` for trivial DoS amplification. `xi_ipc.hpp:306`.

### Adversarial / chaos (Round 2 D)

- **D-P1-1.** Multiple JSON-write paths concatenate strings without escaping (project
  name, plugin name, leader, source name). A `"` in any of these corrupts JSON output.
  `xi_plugin_manager.hpp:1527,1533,1540,1844,1861,1878-1881,1889`.
- **D-P1-2.** `extract_string` / `detail_find_key` substring search matches inside
  string values. A plugin description containing `"plugin": "evil"` causes downstream
  to load `evil`. Brace counter only handles strings on first quote. `xi_plugin_manager.hpp:1624-1667`.
- **D-P1-3.** `iso8601_now` uses `std::gmtime` — not thread-safe on Windows. Two
  concurrent `certify()` race. `xi_cert.hpp:67-72`.
- **D-P1-4.** Plugin DLL load → cert check is TOCTOU. SHA validates DLL_v1, LoadLibrary
  loads DLL_v2 if file replaced between calls. `xi_cert.hpp:140`, `xi_plugin_manager.hpp:861`.
- **D-P1-5.** `atomic_write` return values ignored at 6+ call sites. Disk-full / perm-denied
  silently loses user work. `xi_plugin_manager.hpp:821, 959, 1864, 1899`; `xi_cert.hpp:92`.
- **D-P1-6.** `(size_t)(-n) + 1024` UB on `INT_MIN` — repeated across `worker_main`,
  `script_runner_main`, `runner_main`, `service_main`.
- **D-P1-7/8.** `ImagePool::create` int32 overflow on `w*h*ch`; `live_count_` /
  `total_created_` don't decrement on slot-exhaustion fail → counters drift permanently.
  `xi_image_pool.hpp:91, 103-117`.
- **D-P1-9.** `CancelIoEx(pipe_, nullptr)` cancels concurrent writes too. Watchdog
  timeout mid-burst-parallelism truncates other writer's frame. `xi_process_instance.hpp:374`.
- **D-P1-10.** Trigger bus / watchdog use `system_clock` for deadlines. NTP backward
  skew → AllRequired drops events; forward skew → premature watchdog `TerminateThread`.
  `xi_trigger_bus.hpp:70-74, 199, 333`; `service_main.cpp:3149-3151`.
- **D-P1-11.** WS HMAC bearer mode validates `|now-ts| <= 60s` via `std::time(nullptr)`.
  60s NTP skew rejects valid clients / lets old replay through. `xi_ws_server.hpp:487`.

### Soak (Round 2 E)

- **E-P1-1.** `g_iso_dead_reported` set never cleared. Project A→B→A keeps stale entries;
  if same instance dies again the second death is **not reported** (silent regression).
  `service_main.cpp:336, 341`.
- **E-P1-2.** WS reconnect: `g_sub_*` / `g_history` / `g_iso_dead_reported` /
  `g_recent_errors` survive disconnect. Cross-session contamination — new driver gets
  prior session's subscriptions, history, and error toasts. `service_main.cpp:3118-3120`.
- **E-P1-3.** `xi::async` spawns fresh OS thread per call via `std::async(launch::async)`.
  4 calls × 60 fps = 240 thread cycles/sec = 14.4M/day. ucrt arena fragmentation
  accumulates. Header marks as TODO. `xi_async.hpp:199`.
- **E-P1-4.** `read_exact` / `write_all` create+close `CreateEventA` per chunk per RPC.
  ~360/sec at 60 fps × emit-trigger replies. ~100M handle creations / month. SEH leak path
  if WriteFile/ReadFile raises. `xi_ipc.hpp:227-243, 245-261`.
- **E-P1-5.** `TriggerRecorder::events_` — RAM ~200 MB / 8 h, but **disk 173 GB / 8 h**
  with no rolling/quota. Plus manifest rebuild builds full JSON via `+=` under lock —
  multi-second wedge at 1M events. `xi_trigger_recorder.hpp:172, 175, 179-200`.

### Contract (Round 2 F)

- **F-P1-1.** `run_started` / `run_finished` / `run_error` events documented but never
  emitted. Driver following the doc-suggested `wait for run_finished` loop **hangs
  forever**. Script exception delivers no event and no rsp.error — **silent script
  failures** contradict protocol.md's error-handling promise.
- **F-P1-2.** SDK `compile_and_load` docstring claims rsp returns `{build_log, instances,
  params}`; wire actually returns `{dll, diagnostics, resumed_continuous?}`. `KeyError`
  for any caller using the docstring. `client.py:182-198` vs `service_main.cpp:1528-1533`.
- **F-P1-3.** SDK `compile_and_load` has no `timeout` kwarg (hardcoded 180 s); 30-plugin
  cold-compile dies at 180 s with no escalation path. `client.py:182, 200`.
- **F-P1-4.** SDK `c.call(timeout=…)` is SDK-side only — backend doesn't cancel; on
  timeout SDK raises bare `queue.Empty`, NOT `TimeoutError`. The cmd keeps running
  server-side, mutating state the user thinks was rolled back. `client.py:155-164`.
- **F-P1-5.** `next_vars(timeout)` returns `None` for both timeout AND connection-lost.
  Tail loop spins forever after disconnect with no exception. `client.py:347-363`.
- **F-P1-6.** `c.exchange_instance` doesn't surface the documented `unknown_command`
  shape (we just added the helper but SDK is mute). Driver authors won't learn it from
  `help(c.exchange_instance)`. `client.py:290-306`.

---

## 🟡 P2 — hardening / clarity / lower-frequency

(Listed compactly; see individual round reports for full citations.)

- Manifest validation conflates int/float; nested objects not validated; `extract_string`
  parser fragility on `"isolation"` / `"plugin"` keys appearing inside other JSON values.
- `rename_instance` failure paths leak renamed folders.
- `cmd:save_project` doesn't serialize PluginManager state; two save paths can drift.
- `seq_` `uint32_t` wraps in 828 days at 60 Hz (Round 1 C and Round 2 E both flagged).
- `cmd:shutdown` doesn't `clear_sink` / drain queue → image handle leaks at exit.
- `Pipe::connect()` busy-waits via `Sleep(20)` instead of `WaitNamedPipeA`.
- Worker has no parent-PID watchdog → backend suspended under debugger leaves worker
  hanging open.
- `ProcessInstanceAdapter::shutdown_` calls RPC_DESTROY before `stop_reader_`; 30s
  timeout on hung worker.
- `seq_=0` collision (Round 1 C P0; same as Round 2 E noted).
- `crashdumps/` accumulates with no auto-prune.
- Trigger bus `pending_` / `follower_latest_` evict only on emit path; idle source leaks.
- Pipe name truncated for long instance names; quoting breaks on `"` in path.
- Doc drift: `cmd:start` rsp has undocumented `dispatch_threads` field; `open_project_warnings`
  doc says "planned" but is wired.

---

## Cross-cutting patterns / themes

1. **Hot mutator vs live caller** — Round 1 A + B converged; root of 5 P0s. Pattern:
   handler mutates DLL/instance state without first draining dispatcher pool / in-flight
   calls. Fix is uniform.

2. **Wire-format parsers don't bounds-check user lengths** — `xi_ipc.hpp::Reader` is the
   gap; `recv_frame` and `xi_ws_server.hpp::parse_frame` do bounds-check.

3. **JSON read-substring + JSON write-concat-without-escape** — both directions fragile
   for any string containing JSON metacharacters.

4. **`atomic_write` return values ignored** at 6+ call sites; the fn has correct
   flush-then-rename but contract is unenforced upward.

5. **`(size_t)(-n)` UB pattern repeated verbatim** in 4 files — single helper would
   have caught it.

6. **`system_clock` used for correctness-sensitive deadlines** (watchdog, HMAC replay
   window, AllRequired window) — should be `steady_clock`.

7. **Bump-only allocators / version-only writers** — SHM (P0-E1), script_build (P0-E2)
   both grow unboundedly; explicit "production version would..." in comments.

8. **Doc / impl drift** — events documented but never emitted; SDK docstrings describe
   behaviors that don't exist; protocol doc lists fields the wire doesn't carry (and
   vice versa).

---

## Notes for downstream test plan

- Many P0s are pure timing / ordering issues. Tests need controlled timing injection
  (sleep / delay between specific cmds; thread-count variation; cancel-mid-call).
- Many P1s are doc-vs-impl drift. Tests should read the doc as-is and verify the
  documented behavior, NOT the observed behavior. (E.g. send `cmd:run`, wait for
  `run_finished` event with 5s timeout — currently fails the spec.)
- Several findings are only triggered by adversarial / hostile peer. Tests should
  include a fuzzer pattern: a malicious worker (already exists as `evil_worker.exe`),
  plus a "malicious driver" pattern.
- Soak tests need to be ≥ minutes (P0-E1) or ≥ hours (P0-E2, P1-E5) to surface.
