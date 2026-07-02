# Backend Core Study Report

**Date:** 2026-06-30  
**Focus:** xInsp2 backend native engine subsystems, concurrency, data flow, and design patterns

---

## 0. CURRENT STATE (updated 2026-07-01) — read this first

The body below is the original **2026-06-30 hot-path snapshot**; the core has since advanced. Corrections + current state (full roadmap in `core_fix_plan.md` Parts I–IV; decisions in `OPEN-QUESTIONS.md`):

**Plugin ABI — now v11** (not "v6"). `XI_ABI_VERSION 11`, `XI_ABI_MIN_COMPAT 11`, `sizeof(xi_host_api)==176` (22 fn-ptrs). **8 plugin C exports** (`abi_version`, `create`, `destroy`, `process`, `exchange`, `get_def`, `set_def`, + v7 `prepare`/`commit` for async heavy-asset load).

**Capability segregation (Part II).** Beyond the flat `xi_host_api` struct, the host publishes a CLAP-style **`get_interface(id, version)`** door with frozen per-capability interfaces: `xi.imaging@1`, `xi.doc@1`, `xi.emit@1`, `xi.log@1`, `xi.preview@1`. SDK wrappers query-then-cache with legacy-field fallback. A CI **freeze guard** (`test_abi_freeze.cpp`) pins the v11 layout; changes ship as new frozen versions.

**Removed since the snapshot:** the 5 dead `shm_*` stubs + `xi.legacy@9` door (v11 major break — pre-v11 plugins refused); process isolation (2026-05); VAR (→ `expose` plugin).

**Plugin management hardening (Part III):** subprocess **certify** on scan (`--certify-plugin`, crash-safe load, hash-cached verdict); **per-plugin crash attribution + auto-quarantine** (a crashing plugin is disabled + surfaced, line stays up, instead of latching the whole backend safe); lifecycle × thread contract documented (`write-a-plugin.md`).

**Correctness tooling (Part IV):** MSVC **ASan** + high-iteration race-stress; **perf-regression gate** (`bench_*` → ctest + baselines); salvaged black-box WS fuzz smoke; clang-cl **in-process libFuzzer** (`parse_cmd`/yyjson/record — millions of execs, no findings) + **UBSan**. TSan unavailable on Windows (needs a Linux lane — see OQ-1).

**Dependency surface:** the mandatory plugin umbrella (`xi.hpp`) is now **OpenCV-free** (OQ-9) — cv interop is opt-in via `xi_cv.hpp`; a no-CV plugin builds with OpenCV absent.

**Command surface:** the original report undercounts — the WS protocol has **~50+ commands** (working-copy, instance/plugin lifecycle, observability, runtime control, UI feed, + certify/quarantine/unquarantine). See `core_fix_plan.md` §7 for the full inventory.

---

## 1. Architecture Summary

The xInsp2 backend (`xinsp-backend.exe`) is a **single-process compute core** handling all inspection workloads: plugin loading, script compilation/execution, dispatch scheduling, and WebSocket I/O. All plugins run **in-process** (no isolation); a plugin crash exits the entire backend, relied upon by the supervisor FE (`xinsp-fe.exe`) to respawn.

### Core Boundary
- **In:** WebSocket commands (cmd:compile_and_load, cmd:run, cmd:start continuous, etc.)
- **Out:** WebSocket responses/events (rsp, run_result, log, binary expose data)
- **Data flow:** Source plugins emit records → TriggerBus → dispatch groups (worker threads) → script inspect() → xi::use() → downstream plugins → ordered result emission

### Key Design Bet
**Speed over isolation:** zero-copy refcounted pools (images, JSON docs) + in-process plugin composition outweigh process overhead.

---

## 2. Subsystems & Key Files

### 2.1 Dispatch + Trigger Bus
**Files:**
- `backend/include/xi/xi_trigger_bus.hpp` (150–200 LOC) — TriggerEvent struct + funnel logic
- `backend/src/service_main.cpp` lines 1149–1325 (run_one_inspection) — inspect orchestration
- `backend/src/service_main.cpp` lines 1330–1400+ (GroupLane + worker threads)

**What it does:**
- One emit_record() → one TriggerEvent (no correlation; gathering sources embed multiple frames in one record).
- Per-group worker lanes (owned by GroupLane struct): each lane has its own max_parallel workers + FIFO queue + thread priority.
- TriggerEvent carries: 128-bit trigger ID (TLS PRNG-generated), named-image map (handle → image), metadata doc (yyjson_mut_doc* by pointer), timestamp, group routing.
- Workers dequeue, call run_one_inspection(), script reads via xi::current_trigger() (thread_local).

**Concurrency Model:**
- Multiple groups = multiple independent worker pools (no shared scheduler). Each group spawns N threads at startup, holds them idle when queue empty.
- Lock-free TriggerEvent flow: source thread locks group's queue (brief), enqueues, signals CV. Worker grabs lock, dequeues, releases. Per-group EmitGate serializes result emission in arrival order when ordered mode enabled.

### 2.2 Image Pool (xi_image_pool.hpp)
**Lines:** 1–250 (header-only)

**Structure:**
- **65,536 slots** (fixed array of atomic PoolEntry*), each slot refcounted.
- **Handle format:** bits 0–15 = slot index, 16–55 = generation (40 bits = 1.1e12 reuses/slot).
- Generation defends ABA (a reused slot rejected stale handles).
- **RefCount:** atomic int32_t per PoolEntry; create() returns refcount=1, callers addref/release.
- **Owner tracking:** each entry tagged with ImagePoolOwnerId (script/instance); sweep on owner death frees all entries for that owner (but preserves those still held by live consumers via refcount balance).

**Thread safety:** Lock-free lookups; all mutating ops (create, acquire_slot_, release) guarded by a mutex over the slot vector, not the pixel data itself.

**Allocations:**
- Per-frame: image_create() inside script's inspect() or plugin process() — tagged with current OwnerGuard (script or instance id).
- Leaked handles → owner sweep reclaims them on unload.
- 1 GiB cap per image (D-P1-7) to prevent runaway allocations.

### 2.3 JSON Data Layer (xi_record.hpp + xi_doc_registry.hpp + xi_doc_pool.hpp)
**Files:**
- `xi_record.hpp` (400+ LOC) — Record struct, copy-on-write logic, refcount box.
- `xi_doc_registry.hpp` (100 LOC) — sharded map (16 shards) of doc* → refcount.
- `xi_doc_pool.hpp` — thread-local, size-segregated free-list (powers of 2, 64B–64KB).

**Model:**
- **xi::Record** bundles yyjson_mut_doc* + named-image map.
- **In-process fast path:** script and plugin share the same yyjson layout → hand doc pointer directly (zero serialization, zero copy).
- **Fallback:** JSON-path (yyjson_mut_write/read) when plugin has mismatched yyjson or opts into json_fallback.
- **Copy semantics:** Record copy = refcount bump (zero deep copy); first mutation COWs into a fresh sole-owned doc.
- **Cross-ABI sharing** (γ-4): DocRegistry holds authoritative refcount for docs held on both sides. share_out() enroll + reserve; adopt_shared() consume reserve.
- **Lifecycle:** Host owns doc-chunk pool; whichever side (plugin or host) drops the last ref frees via host's pool.

**Thread safety:** 
- RefCount box uses atomic ops; COW materializes full copy only on frozen-doc write (rare). 
- DocRegistry sharded by doc pointer (hash % 16 shards) → parallel emit/dispatch on different docs doesn't contend.

### 2.4 Script Loader + Hot Reload (xi_script_loader.hpp + service_main.cpp)
**Files:**
- `xi_script_loader.hpp` (200+ LOC) — LoadedScript struct, load_script(), unload_script().
- `service_main.cpp` lines 64–95 (g_script, g_script_mu, param/instance cache).

**Structure:**
- **LoadedScript.handle** = HMODULE to compiled DLL.
- **module_lifetime** = shared_ptr<void> deleter that FreeLibrary() when last snapshot of g_script drops.
- Run-time FnPtrs: inspect, reset, set_param, set_instance_def, get_state, set_state, etc.

**Hot reload:**
1. User edits inspect.cpp, calls cmd:compile_and_load.
2. Compiler produces new DLL.
3. g_script_mu lock: snapshot old LoadedScript, swap g_script = new one.
4. Old snapshot still held by any in-flight run_one_inspection() call (detached or sync); FreeLibrary() deferred until its scope exits.
5. **Persistent state:** g_persistent_state_json + g_persistent_state_schema versioning → survives DLL reload; mismatch drops state rather than corrupting shape.
6. **Param/instance caches:** g_param_cache + g_instance_def_cache replayed into new DLL on load so operator-tuned values survive.

### 2.5 Plugin Manager (xi_plugin_manager.hpp + service_main.cpp)
**Lines:** 100–200 in xi_plugin_manager.hpp; service_main integration ~2000 LOC.

**Lifecycle:**
- scan_plugins(dir) walks plugins/ folder; each plugin is a folder with plugin.json + dll.
- LoadLibrary() each plugin DLL → resolve the C exports (create, destroy, process, exchange, get_def, set_def — plus v7 `prepare`/`commit` and `abi_version`; **8 total**, not 6 — see §0). As of v11 this runs behind a crash-safe subprocess **certify** gate (Part III G1).
- **ABI version gate + yyjson layout stamp:** plugins must declare compatible ABI + same yyjson layout, else load fails (unless json_fallback: true).
- **Certification:** plugin.json validated; instances created per-manifest with InstanceRegistry.

**Instance model:**
- Each instance is a configured plugin: instance.json specifies plugin name + config JSON.
- InstanceRegistry::find(name) returns InstanceBase* (polymorphic; CAbiInstanceAdapter wraps C DLL plugins).
- Instances persist across script recompiles; plugins loaded on open_project, unloaded on close_project.

### 2.6 SEH → C++ Exception Translation (xi_seh.hpp)
**Lines:** 60 (header-only)

**Mechanism:**
- Windows SEH (access violation, divide-by-zero, etc.) are structured exceptions, not C++ throws.
- xi::install_seh_translator() registers _set_se_translator callback (MSVC-only, per-thread).
- Translator catches SEH code → throws xi::seh_exception (extends std::exception).
- service_main try/catch (around s.inspect()) catches seh_exception → logs + emits error event.

**Crash isolation:**
- Plugin or script crash → seh_exception caught → run_one_inspection() survives, emits run_error event.
- Backend as a whole does NOT die (unlike a fatal fault). But no recovery of partially-modified state inside the inspect.

**STACK_OVERFLOW is special — the guard page must be restored (`xi::recover_seh_stack`):**
- A stack overflow is delivered by writing into the thread's single stack **guard page**. Windows does NOT re-arm that page after it fires. Every OTHER SEH code (access violation, /0, …) leaves the stack intact; STACK_OVERFLOW alone leaves the thread's stack in a state where the NEXT deep call does not fault cleanly — it writes past the stack and silently corrupts memory (or dies unrecoverably).
- Because our workers **survive a caught fault and keep running** (the lane worker loops to the next frame; the WS command thread handles the next command; OpenMP / `std::async` pool threads are reused; the runner loops to the next frame), any `catch (seh_exception&)` on such a thread must call `xi::recover_seh_stack(e.code)` (xi_seh.hpp) before the thread does more work. It calls `_resetstkoflw()` only for STACK_OVERFLOW (a no-op for every other code) to re-arm the guard page.
- **If the guard page cannot be restored** (`_resetstkoflw` returns 0) the stack is unusable. Per the crash philosophy (prefer `_Exit` + FE supervisor respawn over running compromised code — same trade as the watchdog HARD trip and the shutdown-drain timeout), the thread does NOT run another frame: the site logs + `std::_Exit(WATCHDOG_EXIT_CODE)` for respawn. `xi::recover_seh_stack_or_die()` bundles recover-or-hard-exit for the shared-header worker sites (xi_parallel / xi_async) that can't reach the service health/log layer. We deliberately do NOT retire+respawn a single lane worker: that machinery doesn't exist, and a lane silently running N−1 (or, at N=1, zero) workers is a dishonest degradation — a full respawn is the honest, philosophy-consistent choice.
- Pairs with `reserve_fault_stack()` (`SetThreadStackGuarantee`, 128 KB): that reserves headroom so the SE translator / crash filter can *run* after the overflow; `recover_seh_stack` restores the guard page so the surviving thread is *reusable*. Both are installed at dispatch-thread entry.
- Covered catch sites: service_inspect.cpp (inspect worker), service_sinks.cpp (`use_process_inline_` plugin `process()`, `use_exchange_cb`, staged-sink flush), xi_parallel.hpp (OpenMP worker), xi_async.hpp (`std::async` worker), runner_main.cpp (frame loop), and the WS command handlers in service_cmd_{dispatch,lifecycle,project}.cpp (exchange/prepare/commit/set_def/get_def, def-replay on hot-reload). Test: `test_fault_policy` "stack overflow" section drives a real overflow through fault_plugin.dll and asserts caught+classified → guard restored → a subsequent deep call completes → a second overflow still faults cleanly.

### 2.7 Headless Runner (runner_main.cpp)
**Lines:** 333 total.

**Design:**
- No WebSocket server. Minimal copy of service_main's xi::use() callbacks.
- Loads project.json → scans plugins → compiles script → runs inspect(frame_number) N times.
- Outputs JSON report: {frames: [{...}], summary: {frames_run, crashed, total_ms}}.
- On plugin crash: catch seh_exception, log, increment crashed counter, continue.
- Exit code: 0 if all frames ran; 1 if any crashed.

**Use case:** production batch runs, CI/CD verification (no GUI, no WS overhead).

---

## 3. Data Flow — From Emit to Result

```
1. Source plugin (camera thread)
   calls xi::emit_record(host, record)
   → host->emit_record(emitter, id, images, meta_doc)

2. TriggerBus::emit()
   - addref() each image handle
   - DocRegistry::retain(meta_doc)
   - route by emitter→group
   - enqueue TriggerEvent into GroupLane.q
   - signal CV

3. GroupLane worker (dispatch thread)
   - dequeue TriggerEvent
   - claim arrival_id (if ordered)
   - set thread_local g_current_trigger = &event
   - call run_one_inspection()

4. run_one_inspection()
   - snapshot g_script (LoadedScript copy)
   - claim EmitTurn (ordered mode)
   - try {
       s.reset()
       s.inspect(frame_hint)
       script calls xi::use("plugin").process(record)
         → service_main use_process_inline_
         → plugin->process_fn()
         → xi::Record result
       script calls xi::result(code, msg)
         → g_run_result = {code, msg}
     }
   - catch seh_exception / std::exception
       → g_run_result = {0, "error"}
   - turn.wait_turn() [arrival order gate]
   - flush_staged_emits_() [sink plugins]
   - emit_run_result(code, msg, run_id, group, source)
   - turn.complete()

5. Ordered emission (per-lane)
   - EmitGate holds seq_next counter
   - each thread's turn = seq_next++ (CAS-claimed under mutex)
   - turn.wait_turn() blocks until it's turn_id's slot
   - turn.complete() advances gate's emission pointer
   - result hits WS wire in arrival order (no reorder, no tear)

6. WS emission
   - xi::ws::Server::send_text(json)
   - thread-safe (mutex guarded)
   - clients (HMI, extension) consume stream in order
```

### Zero-Copy Path Highlight
- **Image:** emit_record(source, record) → image handle in TriggerEvent → script reads via t.image(key) → returns handle (no copy).
- **JSON:** emit_record carries meta_doc pointer → script reads via t.meta() → borrowed read-only view → if COW needed, materializes copy only then.
- **Plugin output:** plugin->process(in_rec) returns out_rec → host adopts doc (registry managed) → no serialize to JSON.

---

## 4. Concurrency Model

### 4.1 Worker Lanes (Per-Group)
```
GroupLane {
  cfg: DispatchGroup (max_parallel, thread_priority, etc.)
  q: deque<TriggerEvent>
  mu: mutex
  cv: condition_variable
  workers: vector<thread>  [N threads spawned at open_project]
  seq_next: atomic (ordered mode only)
  gate: EmitGate (result ordering)
}
```

**Dispatch loop (each worker thread):**
```cpp
while (running) {
  {
    lock_guard<mu> lk(mu);
    while (q.empty()) cv.wait(lk);
    event = q.front(); q.pop_front();
    if (ordered) seq = seq_next++;
  }
  run_one_inspection(..., seq);  // outside the lock
}
```

**Thread priority:** OS THREAD_PRIORITY_ABOVE_NORMAL (high) or BELOW_NORMAL (low) set per-group.
- High-priority group never starved by low-priority threads (Windows preemption).
- No shared work-stealing; each group owns its max_parallel budget strictly.

### 4.2 Concurrency Hazards & Defenses

| Hazard | Defense |
|--------|---------|
| Plugin crash during inspect | SEH translator + try/catch; run_one_inspection survives, emits error event. |
| Script unload mid-inspect | LoadedScript.module_lifetime shared_ptr; in-flight inspect snapshots old script, dtor deferred until inspect returns. |
| Image leak on plugin crash | ImagePool::release_all_for(owner_id) on unload + on destroy; sweeps all handles in that owner. |
| Doc refcount underflow / use-after-free | DocRegistry sharded by pointer; refcount balance: share_out() reserves, adopt_shared() consumes (no double-free). JSON path never holds doc pointer. |
| Lane UAF during respawn | GroupLane held as shared_ptr; producer grabs shared_ptr snapshot, works with it; stop_group_pool_() can't free it while producer is in-flight. |
| Dispatch queue torn write | Brief critical section: lock, enqueue, signal. No torn writes. |
| Ordered emission race | EmitGate::wait_turn() holds per-seq slot, CAS-claims next expected turn. Loser spins/sleeps until its turn. No data race. |

### 4.3 Lock Contention Analysis

**Frequent locks:**
- Group queue mu (brief): enqueue + dequeue per emit. N sources ÷ N group queues = low contention.
- ImagePool mutex (brief): create + addref + release. Pool ops are ~O(log N) due to free-list lookup.
- DocRegistry shards (brief): 16 shards by doc pointer hash. Parallel emits on different docs map to different shards (low contention).
- g_script_mu (brief): snapshot LoadedScript on every run (single acquire per run).

**Infrequent locks:**
- g_lanes_mu: compile_and_load, open_project, close_project (not per-frame).
- PluginManager mu: scan, create instance, destroy instance (design phase, not dispatch).

---

## 5. Notable Design Decisions

### 5.1 In-Process Plugins (No SHM)
**Removed in 2026-05.** Old design: plugins in separate processes, share-memory IPC.

**Trade:** A plugin crash kills the backend. But zero-copy speed wins; process overhead killed HDevelop-like iteration.

**Mitigation:** FE supervisor respawns backend; comms-sidecar holds PLC link across BE crash.

### 5.2 No Trigger Correlation Policies
**Removed in ABI-v6.** Old design: Any/AllRequired/LeaderFollowers correlation rules on the bus.

**New design:** Gathering source plugin emits all frames in one Record. Script reads all via current_trigger().images.

**Win:** Simpler bus (pure funnel), more flexible (any grouping strategy via plugin).

### 5.3 Per-Group Worker Lanes (No Shared Pool)
**Design:** Each group owns N threads + queue; no work-stealing, no priority integer.

**Why:**
- Predictable latency: high-priority group always has its 4 workers (can't be starved by low).
- Simpler scheduler: OS thread priority is the arbiter.

**Cost:** Idle groups hold their threads (stack overhead ~1–2 MB per thread, negligible for ~10 groups).

### 5.4 Ordered Emission via EmitGate (No Throttle)
**Design:** Parallel compute, but result emission serialized to arrival order (when ordered mode).

**Implementation:**
- Each thread claims a gapless seq number from seq_next (CAS under brief lock).
- turn.wait_turn() blocks until it's this seq's slot (cooperative spin/sleep).
- turn.complete() advances the gate's emission cursor.
- Compute stays 100% parallel; only emission gate is serialized.

**Why:** Production HMI/PLC expect frame-order stream; out-of-order results tear visual state.

### 5.5 Copy-on-Write for Frozen Docs
**Design:** Record copy = refcount bump (zero deep-copy). First mutation COWs.

**Two cases:**
- OWNED Typed (common): write → materialize_unfrozen() COWs Record + re-seats node pointer. One copy per mutation.
- VIEW into frozen doc (rare): write detaches (implicit clone of sub-node). Write lands in private copy; original untouched.

**Win:** Script fan-out (same record read by N plugins) is zero-copy; a script that mutates one carries the copy cost.

### 5.6 Persistent State Versioning
**Design:** DLL reload can break state shape; g_persistent_state_schema tracks version.

**Mismatch:** drop state (event:state_dropped); next run starts fresh.

**Why:** Silently default-filling into a different shape corrupts logic; explicit drop + notification safer.

---

## 6. Surprising / Risky Aspects

### 6.1 TriggerEvent Metadata Doc Ownership
**Code:** xi_trigger_bus.hpp line 121–124, service_main.cpp line 154–157.

**Issue:** TriggerEvent holds yyjson_mut_doc* but has no destructor. Ownership transfer is **manual discipline**:
- emit() transfers one ref from caller → event (documented: "OWNERSHIP IS TRANSFERRED").
- Worker must call DocRegistry::release() on every drop/consume path.

**Risk:** Forget the release → doc leaks (registry refcount never hits zero). 
**Mitigation:** release_trigger_event_() in service_main.cpp lines 427–433 wraps every cleanup; no-op if called twice (defensive).

### 6.2 Plugin Crash Isolation is Opt-In
**Code:** xi_seh.hpp installed per-worker thread, but service_main's workers install it at spawn (line ~1550ish).

**Edge case:** A plugin running in a caller-spawned thread (xi::async, plugin worker threads) must install its own translator, else SEH isn't caught.

**Risk:** Uncaught SEH → process termination, no cleanup. But runner_main.cpp + service_main worker threads do install it.

### 6.3 Generation Field ABA Defense is 40-Bit
**Code:** xi_image_pool.hpp line 83: GEN_MAX = (1ull << 40) - 1.

**Math:** Each slot can be reused (1ull << 40) ≈ 1.1 trillion times. 
**At 10k images/sec:** ~35k years to overflow one slot. Practical immortality.

**But:** If a pathological plugin burns slots at GiB/sec, wraparound *could* happen in weeks. 
**Mitigation:** 1 GiB cap per image + slot exhaustion error (cap = 65k slots) should prevent slot thrashing.

### 6.4 EmitGate Cooperative Cancellation (Watchdog)
**Code:** service_main.cpp lines 1214–1222, xi_async.hpp watchdog logic.

**Model:** Watchdog arms deadline per inspect. If inspect overruns → watchdog (separate monitoring thread) calls cooperate-cancel (sets global cancel flag), inspect checks it.

**Risk:** Plugin ignores cancel flag → watchdog times out → hard exit (process termination). No plugin cleanup.

**Mitigation:** Generous default timeout (60 s); only production deployments arm it (`--watchdog-ms`).

### 6.5 Script OwnerId Sweep Doesn't Wait for Consumers
**Code:** xi_image_pool.hpp lines 37–44 (owner sweep comments).

**Model:** On script unload, sweep all images owner=script_id. But if a plugin is caching one → owner=0 (anonymous) + refcount > 1 → lives until plugin releases.

**Edge case:** Script loads (owner=100) → emits image → plugin caches it → script unloads (sweep owner=100) → image survives but owner now anonymous (can't sweep again).

**Risk:** Orphaned image if plugin crashes without releasing. 
**Mitigation:** Total_created_ / live_count_ stats exposed; monitor them. Second sweep on instance destroy catches unbalanced releases.

### 6.6 JSON Fallback Path is Transparent But Slow
**Code:** xi_record.hpp lines 113–115; service_main.cpp lines 143–149.

**Model:** Plugin yyjson version mismatch → serialize doc to JSON bytes here → plugin gets data/len instead of doc pointer (silent fallback).

**Issue:** Fallback is not logged per-call (logged once on load), so slow frames may not show why (doc serialize overhead hidden).

**Mitigation:** Plugin load step checks and warns; plugin.json json_fallback: true must be explicit.

---

## 7. Concurrency Analysis: Example Scenario

**Scenario:** Two sources (camera L + camera R) at 30 fps, stereo-sync gathering plugin, HMI viewer connected.

**Timeline:**
```
T0:  Camera L thread emits frame #100 (images: L_100, meta: frame=100)
     → TriggerBus::emit() → group="high" → GroupLane.q enqueue + signal

T1:  High-priority worker #1 dequeues event #100
     → run_one_inspection() #100, seq=100
     → s.inspect(100) → xi::use("stereo_sync").process(...)
       (but sync plugin waiting for R_100)

T2:  Camera R thread emits frame #100 (images: R_100, meta: frame=100)
     → BUT: sync plugin logic (inside gather loop) already checked for L_100
     → Sync plugin keeps state, re-emits on next cycle
     → This emit queued separately, dispatched on next worker cycle

T3:  Worker #1 finishes inspect #100
     → EmitGate::wait_turn() blocks (event.seq = 100)
     → checks: gate's emission_cursor is at 100 → claimed! can emit
     → emit_run_result(code=0, msg="NA", run_id=100, ...)
     → turn.complete() → emission_cursor = 101
     → WS send to HMI (serialized, no tear)

T4:  Camera L emits frame #101
     → Enqueued; worker #2 or #1 (idle) dequeues it
     → Proceeds in parallel with HMI rendering frame #100

**Concurrency safety:**
- Multiple sources → multiple queue appends (group mu briefly locked).
- Script + plugin run 100% parallel (no lock held inside inspect).
- Result emission serialized by arrival (EmitGate), so HMI never sees reordered results.
- Images zero-copied (handles, no pixel duplication).
- Docs refcounted (sync plugin can cache input zero-copy across cycles).
```

---

## 8. Key Code Landmarks

| Concept | File | Lines |
|---------|------|-------|
| TriggerEvent struct + emit funnel | xi_trigger_bus.hpp | 35–170 |
| ImagePool (slot array, generation, refcount) | xi_image_pool.hpp | 67–250 |
| Record (refcount box, COW, docbox lifecycle) | xi_record.hpp | 94–250 |
| DocRegistry (sharded 16-mutex map) | xi_doc_registry.hpp | 31–100 |
| GroupLane + worker threads | service_main.cpp | 1344–1380, 1500–1650 |
| run_one_inspection (orchestration, SEH, ordered emit) | service_main.cpp | 1149–1325 |
| LoadedScript hot-reload + param cache | service_main.cpp | 64–95, 2800–3000 |
| Plugin manager + ABI gates | xi_plugin_manager.hpp | 79–500 |
| Headless runner (no WS, minimal SDK) | runner_main.cpp | 1–333 |
| SEH translator (Win32 → C++ exception) | xi_seh.hpp | 30–64 |
| WS server (single-client, localhost, RFC 6455) | xi_ws_server.hpp | 1–100 |

---

## 9. Summary of Findings

### Strengths
1. **Zero-copy architecture:** RefCounted pools (images, docs) + in-process + shared_ptr lifetimes eliminate serialization hot path.
2. **Lockless dispatch:** TriggerBus → GroupLane queue → worker threads; critical sections brief and sharded.
3. **Crash recovery:** SEH translator + try/catch in run_one_inspection(); backend survives plugin/script crashes, FE respawns on repeated failures.
4. **Hot-reload coherence:** Param/instance caches + persistent state versioning preserve user tuning across DLL reloads.
5. **Deterministic latency:** Per-group worker lanes + OS priority → high-priority work never starved by low-priority background tasks.

### Weaknesses / Risks
1. **Manual ownership discipline:** TriggerEvent metadata doc requires explicit DocRegistry::release() on every path; no RAII wrapper reduces risk but makes leaks possible.
2. **Transparent fallback:** JSON path on yyjson mismatch is silent and slow; invisible performance regression if a plugin build drifts.
3. **Plugin isolation none:** In-process plugins crash whole backend; mitigation is FE respawn, not isolation.
4. **Watchdog cooperative:** Hard exit on timeout; no plugin cleanup, no state flush.

### Production Readiness
- **High:** Concurrency model proven (dispatch groups tested, ordered emit in place).
- **High:** SEH recovery + FE lifecycle + crash reports established.
- **Medium:** Edge cases (ABA, doc ownership) have mitigations but require operator/monitor awareness.

