# 25 — Red-team data-flow findings (round 2)

Status: **findings only — none fixed yet** (audit 2026-07-05). A second
adversarial load/concurrency pass over the core data flow, run as a 4-way
parallel review (dispatch/lifecycle · pack+pool · record→emit→sink egress ·
ingress/trigger/WS). Companion to the round-1 pass in
[`21-redteam-load-findings.md`](./21-redteam-load-findings.md); items feed
[`19-hardening-backlog.md`](./19-hardening-backlog.md) §A.

**Threat model: BENIGN plugins + BENIGN users.** Only bugs that manifest under
stress / load / races — no malformed input, no malicious peer. Just the
timing/contention corners that only show at scale.

**Verification:** every finding below was re-read against the working tree at
audit time and **re-verified still-present on 2026-07-05** (after the RT8
async-writer, R1 ledger, ws-epoch, and queue_depth/`block` commits landed —
none of which touch these lines).

Severity legend: **P1** = memory-unsafe / data-loss under load · **P2** =
correctness/observability defect a heavy run can hit · **P3** = latent / narrow
/ diagnostic-only.

Not re-reported (known, tracked elsewhere): **RT8** head-of-line send (now
FIXED, doc 23) · **R1** owner-ledger mis-attribution (doc 19 §C; the
`3db0f67` over-release fix is a *different* facet).

---

## Findings

### F1 — `get_bin()` fabricates a `{nullptr, N}` span on pool exhaustion · P1 · memory-unsafe

**Where:** `backend/include/xi/xi_pack.hpp:516` (`Pack::get_bin`) + `:868`
(`TypedPack::get_bin`, identical). Producer side: `PackBuilder::add_bin`
(`:632`) / `TypedPackBuilder::set_bin` (`:1040`).

```cpp
// get_bin, :516
if (e->pooled) return pack_pool::view(e->handle).first(e->inl_len);
```

`add_bin(key, data, n)` with `n >= kPackLargeThreshold (4096)` takes the pooled
branch and **does not check the mint result**:

```cpp
xi_image_handle h = pack_pool::alloc_bytes(data, n);  // pool EXHAUSTED → h == 0
e.pooled = true; e.handle = h; e.inl_len = uint32_t(n);   // {pooled, handle=0, inl_len=n>0}
push(std::move(e), h);   // owned==0 → entry kept, but not tracked in handles_
```

`alloc_bytes` returns `XI_IMAGE_NULL` on exhaustion / bad_alloc
(`ImagePool::create` → 0). No cross-thread race needed — **a build-time burst
that exhausts the pool is enough.** Later any consumer calls `get_bin(key)`:
`pack_pool::view(0)` returns an **empty** span (`:356`), then `.first(inl_len)`
== `.first(n)` on a size-0 span violates `span::first`'s precondition
(`Count <= size()`) → yields `span{nullptr, n}`. Across the ABI,
`f_get_bin` (`xi_pack_abi.hpp:358`) reports **SUCCESS** with `*ptr=nullptr`,
`*len=n`, `return 1` → the consumer reads `n` bytes from `nullptr` → OOB read /
crash / possible info-disclosure (up to ~INT32_MAX).

`get_image` is **not** affected (passes `view(handle)` straight in, no
`.first()`; a null handle yields a truthful length-0 span). A second, rarer
trigger of the same line is a `get_bin` during static teardown after
`g_image_pool_alive == false` (`view()` returns empty there too).

**Fix:** guard the pooled branch —
```cpp
auto v = pack_pool::view(e->handle);
return (!e->handle || v.size() < e->inl_len) ? std::span<const uint8_t>{}
                                             : v.first(e->inl_len);
```
and have `add_bin` treat `h == 0` as a build failure (fall back to inline, or
mark the entry faulted) rather than storing a live-looking pooled entry.

---

### F2 — start/resume window silently drops (and mis-counts) frames · P2 · data-loss + contract break

**Where:** `backend/src/service_cmd_dispatch.cpp:222` (`cmd_start_`) and
`backend/src/service_internal.hpp:351` (`DispatchPoolGuard::resume`), against
`backend/src/service_dispatch.cpp:172`.

Both start and resume flip the live flag **before** the lanes exist:

```cpp
g_eng.continuous = true;         // :222 (start) / :351 (resume)
...
install_trigger_sink_(&srv);
spawn_group_pool_(&srv, interval_ms);   // :233 — lanes populated HERE
```

The bus sink routes on the live flag. In the window between the flag flip and
`spawn_group_pool_` acquiring `lanes_mu`, a concurrent **source emit thread**
reads `continuous == true`, calls `enqueue_to_lane_` → `lane_for_` finds
`g_eng.lanes` still empty → returns `nullptr`:

```cpp
std::shared_ptr<GroupLane> lane = lane_for_(ev.group);
if (!lane) return false;   // service_dispatch.cpp:172 — bare drop
```

The `TriggerEventReleaser` frees the image/doc/pack refs (**no leak**), but the
frame **vanishes silently: no `XI_SYS_DROPPED` marker, no `dropped` counter
bump** — violating the drop-accounting contract every other drop path in this
file upholds. Every lifecycle-op resume (recompile/rebuild/commit/discard/
export via `DispatchPoolGuard`, and `compile_and_load`) rides this same
ordering.

**Fix:** populate `g_eng.lanes` (or hold `lanes_mu`) before flipping
`continuous`, OR route the `!lane` branch through `account_dropped_frame_`
instead of the bare `return false`.

---

### F3 — health-mirror file torn/clobbered under concurrent `health_changed` · P2 · FE reads bad state

**Where:** `backend/src/service_health.cpp:35-52` (`write_health_mirror_`),
triggered via `backend/include/xi/xi_health.hpp:385-387` (`emit_`).

`emit_()` deliberately **unlocks `mu_` before** calling the notifier, so the
mirror write runs with no serialization. `write_health_mirror_` uses a **fixed
`.tmp` name + plain `ofstream`**:

```cpp
std::string tmp = g_health_mirror_path + ".tmp";
{ std::ofstream f(tmp, binary|trunc); f << js; }
std::filesystem::rename(tmp, g_health_mirror_path, ec);
if (ec) { std::ofstream f(dest, binary|trunc); f << js; remove(tmp, ec); }  // NON-atomic fallback
```

**Interleaving:** worker A faults on instance X, worker B faults on instance Y,
concurrently (parallel dispatch → `note_instance_crash_` → `mark_instance_fault`
→ `emit_`). Different names both pass the per-name coalesce → both unlock and
call `write_health_mirror_` at once. A opens `<path>.tmp` trunc; B opens the
**same** tmp trunc → interleaved/clobbered temp. A renames tmp→dest while B
still holds tmp open → on Windows rename fails (sharing violation) → A takes the
`if(ec)` fallback and does a **non-atomic** truncate+write straight onto the
canonical dest while B writes, then `remove()`s the tmp B is using. The FE
(`fe_main.cpp` `read_health_mirror` polls this file) can read a truncated / torn
/ empty health mirror.

Worst realistic case is a **multi-instance fault storm — exactly when the
degraded/fault signal most needs to reach the FE.** Self-heals on the next
`health_changed`, so it's a transient degraded-signal blip, not verdict loss →
P2. This hand-rolled writer is also weaker than `xi::atomic_write` next door (no
`FlushFileBuffers`; non-atomic fallback on any rename error).

**Fix:** wrap `write_health_mirror_` in a static mutex (it's lifecycle-rate —
cost irrelevant) and/or route through `xi::atomic_write` with a per-writer
unique `.tmp` suffix.

---

### F4 — `g_eng.inspect_tid` single global clobbered under `max_parallel > 1` · P3 · false-positive abort (debug/CI)

**Where:** `backend/src/service_internal.hpp:96` (the atomic) with
`backend/src/service_sinks.cpp:450` (ctor store) / `:453` (dtor store 0) /
`:404` (read + abort in `warn_trigger_off_thread_`). *(Independently flagged by
two of the four review lanes.)*

`inspect_tid` is **documented** as a non-thread-local "is a trigger active
*somewhere*?" flag, but implemented as **one** atomic each
`CurrentTriggerScope` sets to its own tid and resets to 0. Under
`dispatch_threads > 1` with `timer fps > 0`:

- Worker A runs a **triggered** frame → `CurrentTriggerScope` sets
  `inspect_tid = tidA (≠0)`.
- Worker B runs a **timer-tick / empty** frame → the else branch runs
  `run_one_inspection` with **no** `CurrentTriggerScope`, so B's thread-local
  `g_current_trigger` stays null.
- B's script calls `current_trigger()` → `trigger_info_cb` sees
  `!g_current_trigger` → `warn_trigger_off_thread_()` reads `inspect_tid ==
  tidA ≠ 0` → concludes "wrong thread" → **`std::abort()` in a debug/asan
  build** (spurious `ERROR` log-once in release) — for a *correctly-behaving*
  script.

The reverse also corrupts it: B entering+exiting its own scope stores
`inspect_tid = 0` while A still has a live trigger, so a genuine off-thread
misuse on A is **missed** (false negative). Trigger conditions are ordinary
(N>1, ticks interleaving with source triggers, a script reading
`current_trigger()` unconditionally). Debug/asan CI can hit the spurious abort.

**Fix:** track a **count** of active trigger scopes (or a per-thread set / set
of owning tids), not a single last-writer tid.

---

## Contingent / needs a targeted probe (not counted above)

### C1 — pack/image accessor vs owner-sweep lifetime UAF · contingent P1 · likely already mitigated

**Where:** `xi_pack_abi.hpp:126-130` (`pack()` returns `&Slot.pack` under `mu_`,
unlocks, the accessor trampoline `:303-389` derefs **after** unlock) racing
`:168-190` (`release_all_for`). Identical shape in `xi_image_pool.hpp:311-341`.

One review lane reported this as P1 "if teardown is non-quiescing"; the
dispatch/lifecycle lane did the fuller trace and concluded the current hardened
lifecycle **prevents it**: `remove_instance`/`rename` never `FreeLibrary`, and
`InstanceRegistry::find` hands out a `shared_ptr` a worker holds across its whole
`process()` — so the adapter destructor (the only caller of `release_all_for`)
cannot run until the worker's ref drops, i.e. no accessor is in flight when the
sweep runs. **My read: mechanism is real but almost certainly unreachable under
the shared_ptr + quiesce discipline.** This is the one point where two lanes
disagreed, so it warrants a targeted stress test — "remove an instance while
another thread is mid-`pack` accessor on that instance's pack" — to confirm or
kill it, rather than a code change now.

---

## New surface not covered by this audit

The `queue_depth:0` **rendezvous** lane and the reintroduced **`overflow:block`**
back-pressure (`service_dispatch.cpp:184+`, doc 24) landed *after* this pass and
are producer-parking paths — exactly the shape most prone to lost-wakeup /
teardown-join interactions. Its own comment flags `DANGER — ONLY safe for a
back-pressure-…` producer. **Recommend a dedicated follow-up pass** over the
`block`/rendezvous wake + stop paths before they carry production load.

---

## Suggested fix order

1. **F1** — memory-unsafe, needs no race, triggers on ordinary burst exhaustion.
   Single-point header guard, low risk.
2. **F2 / F3** — silent data / signal loss under load. Both small, local.
3. **F4** — CI/debug false-abort. Single-point; swap the tid flag for a count.
4. **C1** — write the stress probe; fix only if it reproduces.
5. Schedule the **rendezvous/`block`** follow-up audit (new surface).

Nothing here blocks THE CUT; F1 is the only memory-safety item and is
self-contained.

---

## Independent verification (2026-07-05)

Each finding re-checked against the tree at `4a3b926` by an independent
adversarial pass (one verifier per finding, instructed to REFUTE from the code;
F1 hand-traced). Result: **all four F-findings CONFIRMED; C1 correctly refuted**.

| # | Sev | Verdict | Verification note |
|---|---|---|---|
| **F1** | P1 | **CONFIRMED** | Full chain traced: `add_bin` (`xi_pack.hpp:635`) stores `{pooled, handle=0, inl_len=n>0}` with no `alloc_bytes` check → `get_bin` (`:516`) `view(0).first(n)` on a size-0 span → `f_get_bin` (`xi_pack_abi.hpp:386-388`) returns **rc=1** with `ptr=nullptr, len=n`. No race; a build-time pool-exhaustion burst suffices. `get_image` is safe (no `.first()`). |
| **F2** | P2 | **CONFIRMED** | Both `cmd_start_` (:222) and `resume` (:351) flip `continuous` before `spawn_group_pool_` fills `lanes`; the already-installed sink routes on the plain atomic with no lock across the window → `:172` bare `return false`, no marker/counter. `TriggerEventReleaser` releases (no leak). **Fix nuance:** Option A (populate-before-flip) is unsafe as a naive line-swap — workers gate on `while(continuous)` (:343) so the flip must move INSIDE `spawn_group_pool_` after `lanes` is populated. Option B (route `!lane` → `account_dropped_frame_`) is lower-risk. |
| **F3** | P2 | **CONFIRMED** | `emit_` unlocks before the notifier (`xi_health.hpp:385-387`); fixed `.tmp` + in-place-truncating non-atomic fallback; per-name coalesce lets two concurrent different-instance faults both write. `xi::atomic_write` exists but is unused here. **Fix:** static mutex (primary) + switch to `atomic_write` with a per-writer unique tmp suffix (note: `atomic_write` also uses a fixed tmp, so the unique suffix or the mutex is load-bearing). |
| **F4** | P3 | **CONFIRMED — but the doc's fix is insufficient** | Single global atomic; timer-tick runs the no-scope `else` branch (`service_dispatch.cpp:392-397`); `warn_trigger_off_thread_` aborts on "global tid ≠ 0" without comparing the caller. **A count/set does NOT fix the false-positive** — under N>1 a triggered and a non-triggered inspect legitimately coexist, so any non-empty count still aborts the benign tick. Sound fix must be **relative to the calling thread** (a per-thread "I am a child of a trigger scope" marker propagated into async/OMP children), or at minimum downgrade the debug `abort()` to the release log-once. |
| **C1** | P1? | **REFUTED (unreachable)** | Mechanism is real (accessor derefs the raw `Pack*` after `mu_` unlock vs `release_all_for` erase). But the doc's shared-ptr-across-`process()` reasoning is WRONG for the cross-instance case (the producer's adapter can be destroyed). What actually guards it: `f_emit_pack` takes an **untagged** ref (owner 0) held by `CurrentTriggerScope` for the whole delivery window; it is in no ledger bucket, so `release_all_for(P)` cannot drive `rc` to 0 → the slot is never erased under a live accessor. **Recommend a TSan/ASan probe** (emit_pack from a producer, destroy that producer, concurrently drive `f_get_*` on the consumer) as regression insurance — the safety rests entirely on that untagged-ref accounting staying exactly right. |

**Disposition:** F1 fixed (pooled-branch guard + `add_bin` treats `h==0` as a
build failure); F2 fixed (Option B); F3 fixed (static mutex + `atomic_write`);
F4 fixed (per-thread relative marker, not a bare count); C1 left as-is with a
probe added. See the commits landing these on the polaris2 line.

## Round-2b — `block` / rendezvous / semaphore-queue surface

The producer-parking paths flagged above as "not covered" were audited in a
dedicated follow-up pass (`overflow:block`, `queue_depth:0` rendezvous,
`qa_semaphore_queue`). **Verdict: 2 CONFIRMED / 2 PLAUSIBLE / 1 REFUTED** —
both CONFIRMED items are in the `queue_depth:0` rendezvous path; `overflow:block`
proper and the parked-producer-across-quiesce UAF concern survived refutation.

### RB1 — depth-0 rendezvous `notify_one` for two different waits → a co-producer is lost-woken and wedges · P2 · **CONFIRMED** · FIXED

**Where:** `service_dispatch.cpp` depth-0 branch (producer two-phase wait on
`cv_not_full`: first the slot-free wait `q.empty()||!continuous`, then the
taken-gen wait `taken_count!=t0||!continuous`) vs the worker's single
`cv_not_full.notify_one()` after a pop. With ≥2 `source` instances on ONE
`queue_depth:0` group, a pop's single notify can wake a *first-waiter* (slot-free)
instead of the *second-waiter* whose event was just taken — the taken producer is
then only rescued by a later pop's one-of-N notify, and if the lane goes idle it
stays parked until stop (that source silently stalls). The `taken_count` VALUE is
correct (no mis-attribution); the bug is wakeup DELIVERY. Reachable with a fully
benign config. **Fix (landed):** worker uses `cv_not_full.notify_all()` after a
pop — producers re-check their predicate under the lock, so the mild
thundering-herd is harmless. (Alternatives: a separate cv for the taken-gen wait,
or enforce single-producer for depth-0.)

### RB2 — depth-0 + `max_parallel>1` runs inspections CONCURRENTLY, breaking the advertised "≤1 in-flight" rendezvous; the advisory misdirects · P2 · **CONFIRMED · RESOLVED (Option A: single-worker clamp)**

**Where:** parser advisory `xi_pm_project.hpp` (advisory-only, NOT clamped);
`spawn_group_pool_` spawns `max_parallel` workers regardless of depth; the
producer's taken-gen wait returns at TAKE time (`taken_count` bumped at dequeue,
before `run_one_inspection`). So a depth-0 lane with `max_parallel:4` deposits →
W1 pops (producer unblocks) → deposits again → W2 pops → **W1 and W2 run
inspections concurrently.** Buffer is ≤1; COMPUTE is up to N. This contradicts
doc 24 / every rendezvous comment promising "strict 1-in-flight lock-step," and a
benign user who chose `queue_depth:0` to serialize a non-reentrant script gets a
data race. Worse, the advisory says `max_parallel>1 will sit idle` — FALSE, they
run concurrently — so the guardrail certifies an unsafe config as safe.
Options considered: (a) clamp a depth-0 lane's worker count to 1 in
`spawn_group_pool_` so the 1-in-flight guarantee is REAL (distinct from the
parser config-clamp that was intentionally removed — this makes the runtime
DELIVER the promised semantics); (b) keep depth-0+N as "buffer≤1, N concurrent"
but rewrite the advisory + doc 24 to state the truth (depth-0 bounds queue depth,
not concurrency); (c) release-on-complete N-way rendezvous (large — the
"six problems"; the plugin-semaphore path already achieves the same effect).

**RESOLVED — Option A (maintainer choice).** `spawn_group_pool_` now spawns a
SINGLE worker for any `queue_depth:0` lane regardless of `max_parallel`
(`if (lane->cfg.queue_depth == 0) n = 1;`), so the strict-serial 1-in-flight
rendezvous is genuinely delivered and a non-reentrant script chosen for
serialization is never raced. The config value is left as-set (not the removed
parser config-mutation); the advisory + doc 24 now state the clamp honestly. This
does NOT constrain multi-threaded work — that lives on the plugin-semaphore path
over a normal `max_parallel:N` lane (doc 24 §4 / `qa_semaphore_queue`), which the
core emit gate orders for the sink exactly as before. doc 24's "strict 1-in-flight"
wording now holds unconditionally for depth-0.

### RB3 — `block`/depth-0 stop-wake degrades to a SILENT drop (no `XI_SYS_DROPPED`, no counter) · P3 · **PLAUSIBLE** · fixed alongside F2

**Where:** the depth-0 pre-deposit stop exit and the block stop-wake exit both
`return false`, letting `TriggerEventReleaser` free the event (no leak) with no
`XI_SYS_DROPPED` / counter bump — the exact sibling of F2 on the new parking
paths. Fires only at stop/quiesce (intentional teardown), so low sharpness, but a
frames-in-vs-verdicts+drops-out monitor is short by the parked-producer count at
each hot-reload-under-backpressure. **Fix:** routed through `account_dropped_frame_`
(reason `"dropped: stopped while back-pressured"`), consistent with F2's
resolution.

### RB4 — `qa_semaphore_queue`: a 2nd `source` instance corrupts the process-global `g_sem` (reseed + cross-abort) · P3 · **PLAUSIBLE · example-only**

**Where:** `examples/qa_semaphore_queue/plugins/sem_queue/src/plugin.cpp` — `g_sem`
is a file-scope static shared across instances. Two `source` instances (nothing
enforces one): B's `start_()`→`seed(N)` resets `permits_/max_inflight_` while A
runs (clobbers the in-flight invariant, >N can be admitted); stopping either
calls `abort()` which wakes+false-returns EVERY parked `acquire()` including the
other running source. Worse than the documented permit-leak. **Refuted
sub-candidates:** double-release is harmless (`release` caps `if(permits_<total_)`);
the backlog-drain path hands its permit back (no leak). **Fix (example hardening,
no core change):** refuse a 2nd `source` instance, or make the semaphore
per-owner, + note the shared-static cross-talk in the header.

### RB5 — parked producer holding a `TriggerEvent` across a quiesce → use-after-`FreeLibrary` · P1 if real · **REFUTED**

Killed by two guards: (1) lock-ordered wake — `stop_dispatch_pool_` sets
`continuous=false` THEN takes `lane->mu` to `notify_all(cv_not_full)`; a producer
re-locking on wake synchronizes-with stop's unlock, so it is guaranteed to observe
`continuous==false` and take the drop exit (no park-after-notify window); the
dropped event's handles point into ImagePool/DocRegistry (still live), not the DLL.
(2) plugin producer threads are joined (`~SemQueue→stop_()→join`) before
`FreeLibrary`. Also refuted: the DEFAULT config (drop_oldest, no block) is
behaviour-unchanged (both new branches skipped; only a harmless unused `++taken_count`
+ a notify on a cv no default producer waits on), and the `block` path proper has
no lost-wakeup (single shared predicate, `wait` returns holding `mu` so no
slot-steal).

---

## Round-3 (2026-07-05) — fixes audit · capability/lifecycle · streaming/WS

A third 3-way parallel pass, verified against the tree at `3966748` (i.e. AFTER
the round-2/2b fixes landed). Surfaces: (A) the round-2/2b FIXES themselves —
never adversarially audited; (B) the capability plane + plugin lifecycle
transactional paths; (C) pack streaming reassembly + WS command/dispatch
concurrency. Threat model unchanged (benign plugins/users, stress/load/race only).

**Verdict: 3 CONFIRMED (1×P1, 2×P2) · 5 PLAUSIBLE (1×P1, 4×P3) · many REFUTED.**
The round-2 fixes are CLEAN (0 introduced defects). The one new P1 is a
capability-plane reinit UAF that the round-2 A1/A2 reinit-gate fix did NOT cover.

### A — the round-2/2b fixes (audit): CLEAN

**0 CONFIRMED.** F2, F3, F4, RB1, RB2, RB3 fully refuted; F1 and RB4 refuted on
their primary surface. The fixes introduced no memory-safety or data-loss defect.
Verified in depth: F4's per-thread trigger-ctx marker does NOT leak across pooled
async/OMP threads (`TriggerCtxScope` save/restore + dtor-on-SEH-rethrow), its new
optional export `xi_script_set_trigger_ctx_callbacks` is script-side (not
`xi_host_api`) so `test_abi_freeze` is unaffected, and old-script/old-host
directions are both null-guarded. F3's function-static mutex is one instance
(inline, ODR) and never nests with `HealthRegistry::mu_`. RB2's `==0` clamp is
complete at both parser scopes; nothing downstream sizes on `max_parallel`.
Two narrow **P3** residuals only:
- **RT3-A1 (F1):** a bin `n ≥ 4 GiB` now always takes the inline fallback and
  `write_bin`'s `uint32_t(n)` truncates the length — the msgpack bin32 format
  ceiling, not new logic, and not a benign-load reality. Optional: `$fault` when
  `n > UINT32_MAX`.
- **RT3-A8 (RB4):** `start_()` sets `owns_source_`/the global claim BEFORE
  `std::thread` construction, so a throw during thread-creation-exhaustion leaves
  the claim held → a same-instance `start` retry is refused until an explicit
  `stop_` releases it. Recoverable, example-only. Fix: `if (owns_source_) return;`
  or release on the throwing path.

### B — capability plane + plugin lifecycle: the reinit-gate has two uncovered readers

**Shared root cause.** `reinit()` (`xi_cabi_adapter.hpp:508-548`) destroys the old
`inst_` under the EXCLUSIVE side of `cap_gate_` — but that gate is taken ONLY by
the capability funnel (the round-1/2 A1/A2 fix). The data/lifecycle plane relies on
`CallScope` for reinit-safety instead, and `CallScope` does NOT cover two reachable
readers: the ungated `prepare()` staging entry, and *any* entry on a **reentrant**
instance (`effective_cap_()==0` makes `CallScope` a no-op). The comment
"reinit() serializes itself via the instance's CallScope" (`service_sinks.cpp:71`)
is exactly the assumption that breaks.

#### RT3-B1 — `prepare()` (ungated staging) races `reinit()`'s `destroy_fn_(old)` → UAF · **P1 · CONFIRMED** (hand-verified)
`prepare()` (`xi_cabi_adapter.hpp:426-434`) runs `prepare_fn_(inst_, …)` with NO
`CallScope` and NO `cap_gate_` — by design (staging concurrency contract). A WS
command thread in `cmd_prepare_instance_` (`service_cmd_dispatch.cpp:323`, NOT
quiesced) is mid-`prepare_fn_(P_old)` while a dispatch worker consumes a pending
reinit (`service_sinks.cpp:239` → `apply_pending_reinit_` → `reinit()`), which
swaps `inst_` and frees `P_old` under the exclusive gate — a gate `prepare()`
never holds → **use-after-free on `P_old` mid-`prepare_fn_`**. Preconditions all
benign-reachable: plugin exports `xi_plugin_prepare` (staged frame-perfect swap —
the main intended use), `on_fault=reinit` (opt-in), one caught `process()` fault
arms the pending reinit. `test_prepare_concurrency.cpp` covers prepare-vs-process
but never prepare-vs-reinit. **Fix:** take `std::shared_lock(cap_gate_)` inside
`prepare()` and re-read `inst_` under it (mirrors `f_cap_call`'s shared re-resolve;
blocks only during reinit's brief exclusive destroy, not during `process()`).

#### RT3-B2 — reentrant instance: `run_pack_door()`/`set_def()` race `reinit()`'s destroy (CallScope is a no-op) · **P1 · PLAUSIBLE**
For a reentrant instance `effective_cap_()==0`, so `CallScope` engages nothing.
`run_pack_door()` and `set_def()` (and `reinit()` itself) all take a no-op
`CallScope`, so the data plane's only reinit-guard vanishes: two dispatch workers
on a reentrant `on_fault=reinit` instance can have T2 inside `process(P_old)` while
T3's `reinit()` frees `P_old` → UAF, plus an unsynchronized write/read data race on
the `inst_` pointer itself. PLAUSIBLE (not CONFIRMED) only because it needs a plugin
marked BOTH `reentrant` AND `on_fault=reinit` — semantically odd, nothing warns,
but accepted at load. **Fix:** gate the reentrant data-plane doors on the shared
reinit-gate unconditionally (not via `CallScope`), or refuse `on_fault=reinit` on a
reentrant plugin at load.

#### RT3-B3 — `committed_def_` written ungated by `prepare()` → torn `std::string` · **P2 · CONFIRMED**
`prepare()` writes `committed_def_ = def` ungated (`:432`); `set_def()` writes it
under `CallScope` and `reinit()` reads it. The ungated writer (and the no-op
`CallScope` on reentrant) races a reader → torn read of a `std::string` that can
dereference a reallocated buffer. Same root as B1; closed by the same shared-gate
acquisition in `prepare()`.

*B1/B2/B3 are hard to reproduce without instrumentation — recommend a `stage_probe`
fixture (park inside `prepare()`/`process()` while a second thread drives `reinit()`)
or a TSan/ASan build.*

**REFUTED (B):** machine-provider `evict`/`reload` vs `f_cap_call` (the funnel pins
the adapter `shared_ptr` for the whole handler; erase only decrements, `~adapter`
deferred) · `commit_working_copy` mirror vs live inspect (caller quiesces; mirror
copies scratch→canonical, inspect reads scratch) · `CapRegistry` shadow-stack
(every mutation under the registry `mu_`, `lookup` returns by value, validity is the
reinit-gate's job which `f_cap_call` honours).

### C — streaming + WS/command: 1 CONFIRMED doc gap, rest hardened

#### RT3-C3 — doc-18 "arrival order on one lane" is FALSE under the default `result_order:"completion"` → benign multi-worker lines silently `stream_gap`-abort their streams · **P2 · CONFIRMED**
The ordered-emit gate arms only when `result_order=="arrival" && n>1`
(`service_dispatch.cpp:368`), but the shipped default is `"completion"`
(`xi_pm_project.hpp:298`). Doc 18's "no reorder on one lane" premise (chunk sink
pushes arrive in `$part` order) therefore fails on a stock multi-worker lane: the
staged pushes fire in COMPLETION order, so the doc-18 consumer's `part != next_part`
check aborts every stream with `stream_gap` under load. Masked in QA because
`qa_pack_stream` runs producer AND consumer synchronously inside one `inspect()` —
it never crosses a frame boundary. **Fix:** doc 18 must state a multi-worker
streaming lane REQUIRES `result_order:"arrival"` OR `queue_depth:0` (strict serial,
the RB2 clamp actually makes depth-0 the *safe* streaming shape); ideally the parser
warns on a stream-shaped source with `max_parallel>1 + result_order:"completion"`.

**PLAUSIBLE (C):** RT3-C4 (P3) — a cross-frame `use().process()` DOOR reassembler is
never arrival-ordered even in arrival mode (the gate orders sink PUSHES + run_result,
not the door pull path in the compute half); doc 18 should restrict the cross-frame
consumer to the ordered-SINK role. RT3-C8 (P3) — `cmd:metrics`/`dispatch_stats`
read independent relaxed atomics with no transaction, so a snapshot can see
`frames_total != ok+err`; strictly diagnostic (the header already disclaims
happens-before), a monitor must not assume intra-snapshot consistency.

**REFUTED (C):** propagate_fault still carries `$stream/$part/$eof` (round-1 F1
holds at 3966748) · no host reassembly buffer to overflow (doc 18: consumer buffers,
host never) · `set_def` vs `process` (CallScope blocks, `test_set_def_race`) · param
retune vs read (all `Param<T>` are atomic scalars, no `Param<string>`) · WS async
writer vs command-reply vs run_result (all serialized by `out_mu_`, epoch tag guards
the pop→send swap, CLOSESOCK under `tx_mu_`) · F2 start-window (fixed at this commit).

### Disposition

- **A** (fixes audit): clean — the two P3s optional/example-only.
- **C3** (P2): doc-18 fix + optional parser warn — a documentation/robustness item.
- **B1 (P1) / B2 (P1) / B3 (P2):** ONE shared fix closes all three — acquire the
  shared side of `cap_gate_` inside `prepare()` (+ re-read `inst_`), and gate the
  reentrant data-plane doors on the shared reinit-gate (or refuse reentrant+reinit
  at load).

**STATUS: B1/B2/B3 DEFUSED at load (combo guard); C3 pending; A1/A8 optional.**

B1/B2/B3 are all reachable ONLY via dangerous config COMBINATIONS that nothing
in-tree uses — `on_fault=reinit` together with staged-prepare (`xi_plugin_prepare`)
or a reentrant plugin — plus an actual fault + a race window. Rather than make the
race safe (a `shared_lock(cap_gate_)` in `prepare()` vs `reinit()`'s exclusive gate
— real concurrency work with deadlock-review risk, for a path nothing uses), the
combo is **DEFUSED at the source**: `CAbiInstanceAdapter::set_on_fault` (the single
chokepoint every load/reload path calls) detects `reinit` + (staged-prepare ‖
reentrant) and **downgrades on_fault → reuse** (the already-documented safe fallback)
with a loud stderr warning naming the hazard + doc-25 ref, so `reinit()` never runs
for such an instance and the UAF is unreachable. Header notes added at `prepare()`
and `reinit()` stating the scope of the exclusive gate and that the combo is guarded.
If the combo is ever a genuine need, fix the race (shared `cap_gate_` in `prepare()`
+ gate the reentrant doors) and lift the guard. Landed: `xi_cabi_adapter.hpp`.

**C3 FIXED (doc-18).** doc 18's "Out-of-order tolerance: NONE on one lane" bullet
now carries a "REQUIRED LANE CONFIG" note: a streaming lane MUST be `queue_depth:0`
(rendezvous, single-worker — the RB2 clamp makes it the natural safe shape),
`max_parallel:1`, or `result_order:"arrival"`; a DEFAULT multi-worker completion
lane delivers pushes out of `$part` order and `stream_gap`-aborts every stream.
C4 folded into the same note (a cross-frame reassembler must be a SINK, not a
`use().process()` door). No parser warn added — the parser cannot cleanly tell a
lane will carry a stream, so the doc requirement is the right layer.

A1/A8 remain P3 and optional (a ≥4 GiB single bin / a thread-exhaustion example
retry — neither a benign-load reality); C8 is a diagnostic-only torn aggregate read
(doc caveat at most). None scheduled.

