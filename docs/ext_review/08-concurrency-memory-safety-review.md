# xInsp2 Concurrency and Memory-Safety Review

| Field | Value |
|---|---|
| Date | 2026-07-02 |
| Reviewer | Claude (external advisory) |
| Status | Advisory |

Related reviews:

- [`02-core-and-developer-ux-review.md`](./02-core-and-developer-ux-review.md) (ownership model, ImageRef, read-only input)
- [`05-real-time-performance-determinism-review.md`](./05-real-time-performance-determinism-review.md) (observer isolation, `xi::async`, ordered-gate wait)
- [`00-triage.md`](./00-triage.md) (maintainer decisions — not re-litigated here)

## Scope

A code-level audit of the compute core's hot path for **concurrency correctness and memory safety only** — not UX, not performance policy. Concretely:

- the refcounted `ImagePool` and the `DocRegistry` / `DocChunkPool` document pools (leak / double-free / ABA / refcount atomicity);
- `thread_local` trigger and image-pool-owner state across the dispatch thread, OpenMP workers, and `xi::async` tasks;
- hot-reload lifecycle (in-flight frames, `xi::use()` persistent objects, DLL unload vs live callbacks);
- crash-isolation mechanics (SEH translation) — specifically *what state can be left inconsistent and then reused after a caught fault*;
- dispatch / worker lifecycle (shutdown races, queue teardown);
- the WebSocket send path under parallel emission;
- data races on shared counters, stats, and connection state;
- OpenMP + `std::async` interaction.

Sources read in full: `xi_image_pool.hpp`, `xi_doc_pool.hpp`, `xi_doc_registry.hpp`, `xi_trigger_bus.hpp`, `xi_seh.hpp`, `xi_crash_dump.hpp`, `xi_async.hpp`, `xi_parallel.hpp`, `xi_inflight_runs.hpp`, `xi_emit_gate.hpp`, `xi_owner_lock.hpp`, `xi_cabi_adapter.hpp`, `xi_script_loader.hpp`, `xi_ws_server.hpp`, and the dispatch / lifecycle / crash / result-emit spans of `service_main.cpp`. Findings distinguish **CONFIRMED** (full path traced) from **PLAUSIBLE** (suspicious, not fully traced).

## Executive Summary

The core is unusually careful about the hard cases. The image-pool handle scheme is lock-free with a versioned ABA defense; the per-frame refcount discipline is wrapped in RAII types (`CurrentTriggerScope`, `TriggerEventReleaser`, `StagedEmitGuard`, `DocRef`, `RecordOutGuard`, `ImagePoolOwnerScope`) whose comments show the exact bugs they were built to kill. Hot-reload uses a shared-ownership `module_lifetime` so a DLL is never unmapped under an in-flight inspect. Shutdown uses a correct Dekker handshake (`InflightRuns`) plus a hard-exit fallback when a wedged inspect won't drain. The watchdog deliberately refuses to `TerminateThread` (which would leak the per-instance gate and risk heap corruption) and instead `_Exit`s for supervisor respawn. The SEH boundary correctly releases the per-instance `CallScope` gate on a caught fault because it is RAII. This is a mature, well-defended hot path.

The residual risks cluster in three places the per-frame RAII discipline does **not** reach:

1. **Diagnostic / management reads that walk the whole pool or touch the connection without the synchronization the hot path uses.** The `ImagePool` stats walk and the WebSocket `client_` teardown are both real data races that can fault the backend from otherwise-benign operations (polling stats, a client disconnect).
2. **Threads the core did not spawn.** A raw `#pragma omp parallel` region and any plugin-spawned worker run without the SEH translator and without owner tagging, so a fault there kills the whole backend and images they create leak. The blessed `xi::parallel_for` / `xi::async` wrappers fix this, but nothing prevents or detects the raw form.
3. **State that survives a caught fault.** A plugin instance whose `process()` is caught mid-mutation is reused with no quarantine; and the per-thread crash-breadcrumb table leaks a slot per distinct thread id, degrading crash attribution to a racy shared slot after 64 lifetime threads.

None of these undermine the steady-state per-frame path. All of them are reachable in normal operation (a UI polling pool stats, a client reconnecting, a script using OpenMP, a long-running one-shot deployment). They are the right next tier of hardening.

## Scorecard

| Dimension | Grade | Rationale |
|---|:--:|---|
| Image pool: core refcount + handle algorithm | B+ | Lock-free, ABA-defended, atomic refcounts; correct under the "hold a ref" contract |
| Image pool: diagnostic / sweep slot walks | C | `stats()` / `stats_by_owner()` deref freed entries — UAF against concurrent `release()` |
| Doc pools (`DocRegistry` / `DocChunkPool`) | A- | Sharded locks, RAII `DocRef`, correct cross-thread free; no issue found |
| `thread_local` trigger / owner discipline | B | Strong RAII + off-thread abort guard; raw-OpenMP path escapes it |
| Hot-reload lifecycle | A- | `module_lifetime` shared ownership + quiesce/drain; in-flight-safe |
| Crash isolation (SEH-translated threads) | B+ | Gate released on catch; `_Exit` on hard trip; dumps on every CRT death path |
| Crash isolation (untranslated / stateful) | C+ | Raw-omp fault kills backend; breadcrumb slots exhaust; plugin state reused |
| Dispatch / worker shutdown ordering | A- | Dekker handshake, drain, hard-exit fallback; no teardown UAF found |
| WS send path concurrency | C+ | Sends serialized by `tx_mu_`, but `client_` closed off-lock — race + UAF window |
| Shared counters / lock ordering | B+ | Counters atomic; no deadlock or lock-order inversion found in core |
| **Overall** | **B** | Excellent hot-path discipline; hardening gaps in diagnostics, foreign threads, and post-fault state |

## Findings

### 1. `ImagePool::stats()` / `stats_by_owner()` dereference pool entries freed concurrently by `release()` — UAF read

**Evidence.** The pool's own contract is explicit (`xi_image_pool.hpp:24-29`): *"when accessing data()/width()/etc on a handle, the caller MUST hold a refcount … another thread's release() can free the underlying PoolEntry mid-deref."* The diagnostic walks violate exactly this contract because they cannot hold a ref on every slot:

```cpp
// xi_image_pool.hpp:342-352  stats()
for (uint32_t i = 0; i < SLOT_COUNT; ++i) {
    PoolEntry* e = slots_[i].entry.load(std::memory_order_acquire);
    if (!e) continue;
    if (owner != 0 && e->owner != owner) continue;   // read e->owner
    ++s.handle_count;
    s.total_bytes += e->pixels.size();                // read e->pixels — deref
}
```

`stats_by_owner()` (`:359-373`) is identical, and `release_all_for()` (`:300-323`) reads `e->owner` before its `fetch_sub`. Concurrently, `release()` frees the entry with no coordination:

```cpp
// xi_image_pool.hpp:178-184
if (e->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    slots_[idx].entry.store(nullptr, std::memory_order_release);
    release_slot_(idx);
    delete e;                                          // frees under the walker's pointer
    live_count_.fetch_sub(1, std::memory_order_relaxed);
}
```

These run on different threads. `cmd_image_pool_stats_` (`service_main.cpp:3692-3693`) calls both walks on the command/serve thread, while lane workers release pool handles continuously during a run — every `CurrentTriggerScope` destruction releases the frame's images (`service_main.cpp:553`), and every script output/expose handle is released after use.

**Failure scenario (CONFIRMED).** A run is in continuous dispatch. The VS Code UI (or any monitor) polls `cmd:image_pool_stats`. The serve thread loads `e = slots_[i].entry` (non-null, passes the null check), and before it reads `e->pixels.size()`, a lane worker releases that image's last ref → `delete e`. The serve thread now reads `pixels.size()` and `owner` through a freed heap pointer: garbage byte counts at best, an access violation (caught by SEH → surfaced as a spurious backend fault, or a minidump) at worst. There is additionally a benign-looking but formally-UB data race on the non-atomic `e->owner`, which `release_all_for()` also *writes* (`:318 e->owner = 0`) while the stats walk reads it.

This is the highest-ranked finding because it turns a read-only observability call into a memory-safety hazard, and observability calls are issued by default whenever a client is attached.

#### Recommendation

Give the pool a coherent snapshot mechanism instead of an unlocked walk. Options in increasing cost: (a) maintain the per-owner aggregates (`handle_count`, `total_bytes`) as atomics updated in `create` / `release` / `release_all_for`, so stats is a read of counters and never touches `PoolEntry`; (b) copy `{owner, size}` out under a short per-slot spin only where `entry != nullptr`, accepting that a slot freed mid-copy is skipped; or (c) a `shared_mutex` taken in shared mode by the walkers and in exclusive mode around the `delete e` in `release` (reintroduces the lock the lock-free design removed, so measure). Option (a) is the speed-first choice and also fixes the `live_now`/`total_bytes` tearing. Whichever is chosen, `e->owner` must stop being a plain field mutated on one thread and read on another — make it `std::atomic<ImagePoolOwnerId>` or fold it into the snapshot.

### 2. WebSocket `client_` is closed off-lock while workers send under `tx_mu_` — data race and use-after-close window

**Evidence.** `send_frame` is correctly serialized: it takes `tx_mu_`, checks `client_`, and sends the whole frame under the lock (`xi_ws_server.hpp:717-758`). But the socket is *closed and nulled* by the poll thread with no lock at all:

```cpp
// xi_ws_server.hpp:429-434  close_client() — no tx_mu_
void close_client() {
    if (client_ != INVALID_SOCK) {
        CLOSESOCK(client_);
        client_ = INVALID_SOCK;   // written on the poll thread, unsynchronized
        ...
```

`close_client()` runs on the poll/serve thread (`:288`, `:389`) whenever a read fails or the connection drops; `send_frame` runs on every dispatch worker emitting a result (`emit_run_result` / `emit_run_event_` → `srv.send_text`). The accept path also writes `client_ = s` off-lock (`:350`). So `client_` — a plain `socket_t` — is written on the poll thread and read/used on N worker threads that hold a *different* discipline (`tx_mu_`).

**Failure scenario (CONFIRMED as a data race; use-after-close is platform-dependent).** A worker enters `send_frame`, takes `tx_mu_`, passes `if (client_ == INVALID_SOCK)`. The client disconnects; the poll thread runs `close_client()` and `CLOSESOCK(client_)` — it does not wait on `tx_mu_`, so it proceeds concurrently. The worker then calls `::send(client_, …)` on a socket the OS has just closed. On the benign path `::send` returns an error and `drop()` runs. On the harmful path the OS has reused that socket/handle value for a new connection or another fd opened in the interim, and the worker writes WebSocket frame bytes into an unrelated destination. Even absent reuse, the read of `client_` racing the write is UB, and `drop()`'s own `::shutdown(client_, …)` (`:745`) can fire on `INVALID_SOCK` or a reused handle.

The single-client localhost design makes the reuse window small but not absent (remote-backend mode over LAN widens it, and reconnects are routine). The comment at `:743` ("The poll thread owns the actual close (avoids an fd-reuse race with recv)") shows the reuse hazard was considered for `recv` but the symmetric hazard against `send_frame` under `tx_mu_` was not closed.

#### Recommendation

Bring the close under the same lock the senders use, or decouple the fd's lifetime from the sends. Minimal fix: make `close_client()` (and the accept-time `client_ = s`) take `tx_mu_` around the mutation, and make `client_` reads in `send_frame` a single snapshot under the held lock. Cleaner: make `client_` a `std::atomic<socket_t>`, have the poll thread only `::shutdown()` (never `CLOSESOCK`) so an in-flight `::send` fails cleanly, and defer the actual `CLOSESOCK` until no send is in flight (e.g. a send-in-progress counter checked by the poll thread before close). Do not close a socket that a worker may be mid-`::send` on.

### 3. Crash-breadcrumb slots leak per distinct thread id; after 64 lifetime threads, attribution collapses to a racy shared slot

**Evidence.** Each thread claims a fixed breadcrumb slot on first use and **never releases it** (`xi_crash_dump.hpp:91-111`):

```cpp
for (int i = 0; i < kMaxSlots; ++i) {           // kMaxSlots = 64
    uint32_t expected = 0;
    if (g_slot_tid[i].compare_exchange_strong(expected, tid, ...)) {
        t_idx = i; g_slots[i].thread_id = tid; return g_slots[i];
    }
}
return g_slots[0];   // slots exhausted → shared, racy fallback
```

A slot is claimed only against an *empty* (`tid == 0`) slot and is never reset when the owning thread exits. The dispatch model produces a steady stream of distinct thread ids: `stop_dispatch_pool_` joins and destroys the lane workers on every `cmd:stop` / reload and `spawn_group_pool_` creates fresh ones; more sharply, every non-continuous frame runs on a brand-new detached thread — `dispatch_one_shot_` → `InflightRuns::launch` spawns `std::thread(...).detach()` per emit (`xi_inflight_runs.hpp:73`, `service_main.cpp:1929`). Windows recycles thread-id *values*, but a recycled id does not reclaim its old slot (the loop only fills `tid == 0` slots), so distinct-id count grows monotonically toward 64 and then saturates.

**Failure scenario (CONFIRMED).** A one-shot-dispatch deployment (source emits, continuous mode off) runs 64+ frames. Every subsequent inspect thread falls through to `g_slots[0]`. Now multiple live inspect threads write `last_cmd` / `last_plugin` / `last_instance` into the *same* `Context` with plain `strncpy` (`:113-117`) — precisely the cross-thread clobber the per-thread slots were introduced to prevent (`:81-86`). A crash at this point produces a report that blames whatever plugin last wrote slot 0, which may be a different thread's plugin. Worse, the `threads[]` array in the report (`:345-365`) still lists the 64 dead threads' stale breadcrumbs as if live, because their slots were never cleared. The `culprit` cross-check against the faulting module (`:140`) limits *mis*-attribution of a core/script crash to a plugin, but per-thread `last_plugin`/`last_phase`/`last_status` become unreliable — degrading exactly the forensic feature (`cmd:crash_reports`, FE crash history) this module exists to serve.

#### Recommendation

Release the slot when the owning thread exits so the table tracks *live* threads, not lifetime threads. A `thread_local` guard whose destructor stores `0` into `g_slot_tid[t_idx]` (and zeroes the `Context`) reclaims the slot on thread exit; this is the same pattern `DocChunkPool::Heads` already uses to reclaim thread-local pool blocks (`xi_doc_pool.hpp:123-131`). Separately, prefer a bounded, reused worker pool for one-shot dispatch over a thread-per-frame model — it would bound thread-id churn for this table, the doc-pool thread-locals, and the SEH-translator install cost alike (this dovetails with review 05 #14's bounded-executor recommendation).

### 4. A raw `#pragma omp parallel` region bypasses the SEH translator and owner tagging — a fault there kills the backend and its images leak

**Evidence.** The SEH translator is per-thread on MSVC and is installed only on threads the core spawns: lane workers (`service_main.cpp:1804`), the one-shot thread (`:1931`), and inside the `xi::async` closure (`xi_async.hpp:311`). The OpenMP runtime spawns its own pool threads with none. `xi::parallel_for` closes this precisely — it installs the translator and re-installs the owner per OpenMP worker, and catches everything inside the region (`xi_parallel.hpp:74-100`), and its own header states the hazard it exists to prevent: *"a hardware fault … inside a raw omp region is not converted to a catchable xi::seh_exception and TERMINATES THE WHOLE BACKEND"* (`:11-16`). Nothing, however, forces a script author to use the wrapper.

**Failure scenario (CONFIRMED gap; trigger is user code).** A script writes an ordinary `#pragma omp parallel for` (the natural thing to write, and what the ecosystem's OpenMP examples model) and dereferences a bad pointer inside it. The fault raises on an OpenMP worker thread that has no `_set_se_translator`, so it is not turned into `seh_exception`; it escapes every `try/catch` in `use_process_inline_` / `run_inspection_compute_` and reaches the process-level `write_minidump` → the entire backend dies instead of the fault being isolated to one frame. Secondarily, any `image_create` on those OpenMP workers is tagged `owner = 0` (the `OwnerGuard`'s `thread_local` is set only on the inspect thread, `service_main.cpp:1370`), so `release_all_for(script_owner)` on unload never sweeps them — a per-frame handle **leak** that accumulates until the 65 536-slot pool is exhausted and `create()` starts returning 0.

**Confidence.** CONFIRMED that a raw region bypasses both mechanisms; the *impact* depends on whether a given script uses the raw form. Because crash isolation is a headline guarantee, a silent hole in it that depends on the author avoiding a standard C++ construct is a real correctness risk.

#### Recommendation

Do not rely on convention for a safety boundary. Two complementary moves: (a) install the SEH translator (and reserve fault stack) once per OpenMP team regardless of entry path — e.g. an `omp_set_…`-style init the host runs when `_OPENMP` is active, or a lint/compile-time check that flags raw `#pragma omp` in `inspect.cpp` and steers to `xi::parallel_for`; (b) surface the owner-leak: the per-script sweep already reports `swept N leaked handles` (`xi_cabi_adapter.hpp:304`) — treat a nonzero sweep on a script that used OpenMP as a diagnostic, not a silent reclaim. At minimum, document the raw-omp hole where authors will see it (the script guide's Parallelism section), not only in `xi_parallel.hpp`.

### 5. A plugin instance is reused after a caught `process()` fault with no quarantine — corrupt-but-not-fatal state can persist

**Evidence.** When a plugin's `process()` faults, the SEH exception is caught in `use_process_inline_` (`service_main.cpp:277-287`), which logs, calls `note_instance_crash_`, and returns `-2`. The instance object, its `xi::use()`-persistent state, and its owner bucket are all left in place; the next frame calls the same instance again. The catch correctly unwinds the RAII `CallScope` gate and `OwnerGuard` (so no lock leak, no owner-tag leak), and genuine heap corruption (`STATUS_HEAP_CORRUPTION 0xC0000374`) or a fastfail is non-continuable and takes the process down via `write_minidump` (`xi_crash_dump.hpp:455-463`) rather than being "recovered". So the core's *own* invariants survive. What does not is the plugin's *own* persistent state: a fault that occurs after the plugin has partially mutated an internal structure (a half-updated model, a container resized but not filled) leaves that structure inconsistent, and the instance is reused with it.

**Failure scenario (PLAUSIBLE / partly by-design).** A stateful plugin (accumulator, tracker, model holder) throws/faults midway through updating its persistent state under `xi::use()`. The frame is reported as a crash and dropped (correct). The next frame re-enters the same instance, reads the half-updated state, and produces silently-wrong results (not a crash, so nothing flags it) until the state happens to be overwritten cleanly or the plugin faults again. There is a `note_instance_crash_` breadcrumb for crash-loop alerting, but no automatic quarantine / re-instantiation of a plugin that faulted mid-mutation.

**Confidence.** PLAUSIBLE — the reuse is real and traced; whether it produces wrong output depends on the plugin's state model. This is arguably acceptable under the "plugins are trusted" posture, but it is worth stating as an explicit residual: caught-fault recovery restores *core* invariants, not *plugin* invariants.

#### Recommendation

Offer (not impose) a state-safety contract for stateful plugins: on a caught `process()`/`exchange` fault, mark the instance "faulted" and, on the next entry, re-instantiate it from its last committed `get_def()` (dropping in-flight persistent state) rather than reusing possibly-inconsistent state — the hot-reload machinery already knows how to serialize/restore instance state, so this reuses existing capability. Gate it behind a per-plugin opt-in so a plugin that genuinely tolerates partial state can decline. At minimum, escalate a repeated per-instance crash (the `note_instance_crash_` count) to a hard quarantine so a crash-looping instance stops being re-entered every frame.

### 6. `TriggerBus::emit` fires the sink outside the bus lock; a concurrent `clear_sink` during teardown could run the sink against tearing-down state

**Evidence.** `emit` snapshots the sink under `mu_` and then invokes it after dropping the lock (`xi_trigger_bus.hpp:196-205`):

```cpp
Sink to_fire;
{ std::lock_guard<std::mutex> lk(mu_); to_fire = sink_; }
if (to_fire) { to_fire(std::move(ev)); }   // fired with mu_ released
```

Releasing the lock before firing is deliberate and correct for avoiding a deadlock (the sink re-enters dispatch). But it means a copy of the sink functor can be executing on a source's emit thread while another thread calls `clear_sink()` (`:114-117`) during a lifecycle op or shutdown.

**Failure scenario (PLAUSIBLE, appears mitigated).** The installed sink (`service_main.cpp:2072`) captures a raw `srv` pointer and routes to `dispatch_one_shot_` / `enqueue_to_lane_`. Both callees re-check the relevant guard — `enqueue_to_lane_` bails on `!g_eng.continuous` after taking the lane lock (`:1713`), and `dispatch_one_shot_` goes through `InflightRuns::launch`, whose Dekker handshake bails if `shutting_down()` (`:1929`, `xi_inflight_runs.hpp:71`). `controlled_shutdown_teardown_` also stops emit sources and `begin_shutdown()`s before dropping `srv`. So the captured `srv` is kept alive across the window in the traced paths, and the sink's side effects are individually guarded. I did not find a concrete UAF, but the pattern (fire an unbounded user-reachable callback with a raw captured pointer, outside the lock that protects sink lifetime) is the kind that regresses silently when a new sink or a new teardown ordering is added.

**Confidence.** PLAUSIBLE — no exploitable path traced; flagged as a fragility, not a confirmed bug.

#### Recommendation

Keep firing outside the lock, but make the sink's lifetime explicit rather than implied: capture `srv` as a `shared_ptr`/weak handle the sink locks per call, or gate the sink body on a single "engine live" atomic checked immediately inside the functor. This makes the safety of "sink runs after `clear_sink` began" a property of the sink, not of the current set of downstream guards.

## Notable strengths (for calibration — not weaknesses)

These were audited and found sound; they are the reason the overall grade is B and not lower.

- **Refcount atomicity and ABA.** `PoolEntry::refcount` uses `acq_rel` on the decrement (`xi_image_pool.hpp:178`); the free-list is a versioned Treiber stack (`:753-793`); the 40-bit per-slot generation defeats stale-handle ABA in `lookup` (`:95-105`). The exhaustion path saturates `next_fresh_` so a full pool cannot wrap and alias a live slot (`:768-777`).
- **Owner sweep is refcount-correct.** `release_all_for` drops exactly one ref per owned entry and spares still-referenced entries (orphaning `owner → 0`) rather than force-deleting them (`:300-323`) — the fix for the cross-instance zero-copy UAF documented inline.
- **Per-frame RAII discipline.** `CurrentTriggerScope` (`service_main.cpp:566-581`), `TriggerEventReleaser` (`:590-597`), `StagedEmitGuard` (`:607`), `RecordOutGuard` (`:627-645`), and `DocRef` (`xi_doc_registry.hpp:126-157`) make "release on every early-return / exception / stop-wake" the default; the compute path's three catch arms all disarm the watchdog and record outcome uniformly (`service_main.cpp:1414-1439`).
- **Hot-reload is in-flight-safe.** `run_inspection_compute_` snapshots the `LoadedScript` under `script_mu` (`:1293`), and `module_lifetime` (a `shared_ptr<void>`) defers `FreeLibrary` + owner-sweep until the last in-flight copy drops (`xi_script_loader.hpp:40-50`); lifecycle ops quiesce and drain before unloading DLLs (`quiesce_dispatch_for_lifecycle_op_`).
- **Shutdown ordering.** `InflightRuns` implements a correct bump-before-detach / bail-if-shutting / drain-on-teardown handshake (`xi_inflight_runs.hpp`), and `controlled_shutdown_teardown_` hard-exits rather than risk a FreeLibrary-vs-in-flight UAF when a wedged inspect won't drain (`service_main.cpp:1993-1998`).
- **Crash-path robustness.** Every CRT death path (terminate, abort, invalid-parameter, purecall) is intercepted and re-raised so a dump is always written (`xi_crash_dump.hpp:392-441`); fault stack is reserved so a stack-overflow can still dump (`:232-235`); the SEH `CallScope` gate is released by unwinding on a caught fault (`xi_cabi_adapter.hpp:442-446`).
- **`xi::parallel_for` and `xi::async`** correctly carry the SEH translator, the owner id, and the watchdog cancel ticket onto worker threads and confine exceptions to the spawning thread (`xi_parallel.hpp:74-125`, `xi_async.hpp:302-335`). Finding 4 is that the *raw* form bypasses this, not that the wrappers are wrong.
- **WS framing** is serialized by `tx_mu_` across the whole frame (`xi_ws_server.hpp:717-758`) — finding 2 is specifically the off-lock *close*, not the send serialization.

## Prioritized Roadmap

### P0 — memory-safety races reachable in normal operation

1. Fix the `ImagePool` diagnostic walk (Finding 1): atomic per-owner aggregates or a coherent snapshot; make `PoolEntry::owner` atomic.
2. Fix the WS `client_` teardown race (Finding 2): close under `tx_mu_` / atomic fd / shutdown-then-deferred-close.

### P1 — degradation of safety mechanisms over uptime

3. Reclaim crash-breadcrumb slots on thread exit and reuse a bounded worker pool for one-shot dispatch (Finding 3).
4. Close the raw-OpenMP SEH/owner hole — per-team translator install and/or a compile-time steer to `xi::parallel_for`; surface owner-sweep leaks (Finding 4).

### P2 — post-fault state hygiene

5. Add opt-in instance re-instantiation / crash-loop quarantine after a caught `process()` fault (Finding 5).
6. Make the trigger-sink lifetime explicit at the firing site (Finding 6).

## Decision Checklist

- Can a read-only management call (`image_pool_stats`, `dispatch_stats`) ever dereference state a worker can free concurrently? (Today: yes — Finding 1.)
- Is every socket close serialized with every socket send? (Today: no — Finding 2.)
- Does a safety mechanism (breadcrumbs, SEH translator, owner tagging) degrade with process uptime or with a standard C++ construct in user code? (Today: yes — Findings 3, 4.)
- After a *caught* fault, is the reused state limited to state the core knows is consistent? (Today: core yes, plugin no — Finding 5.)
- Is any user-reachable callback fired outside the lock that guards its lifetime, with a raw captured pointer? (Today: the bus sink — Finding 6.)
- Is `PoolEntry::owner` (written by `release_all_for`, read by stats) synchronized? (Today: no — Finding 1.)

## Final Judgment

The per-frame hot path is genuinely well engineered: the refcount algorithm, the RAII release discipline, the hot-reload lifetime model, and the shutdown handshake are all correct under scrutiny, and the crash-isolation design makes the right conservative call (exit for respawn rather than kill a thread holding a lock). The core does not have a systemic concurrency problem.

The exposure is at the edges the hot-path discipline was not extended to: diagnostic reads that walk the pool without a ref (Finding 1) and a connection teardown that races the senders (Finding 2) are the two that can fault a running backend from ordinary actions, and should be fixed first. The remaining findings are slower-acting — a safety mechanism that erodes with uptime (Finding 3), a crash-isolation guarantee with a user-triggerable hole (Finding 4), and post-fault plugin-state reuse (Finding 5). Fixing the two P0 races and reclaiming breadcrumb slots would move this from "excellent hot path with sharp edges" to "uniformly safe under concurrency," without touching the speed-first design or the frozen ABI.
