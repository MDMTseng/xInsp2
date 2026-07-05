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

---

## Round-3b — app-team parallel fresh-surface sweep (G1–G5), independently verified

A SECOND round-3, run in parallel by the app team over surfaces the A/B/C sweep did
not reach (the RT8 writer teardown, the reentrant capability self-heal, the
parallel/async primitives, and the SCRIPT-instance / `xi::kv` state plane). Merged
here; every finding re-verified against `e1b95b9` by an independent verifier. It is
strongly complementary — it found a P1 (G2) the A/B/C sweep missed and a defect (G5)
on the RT8 writer itself.

**Verdict (verified): G1 RESOLVED-by-guard · G2 CONFIRMED P1 · G3/G4/G5 CONFIRMED P2.**

### G1 — cap-plane concurrent `reinit()` double-free / `inst_` data race · P1 · **RESOLVED (= B1/B2, defused by the combo guard)**
Two threads in `f_cap_call` for the same reentrant provider both read `reinit_pending`
true before either clears it (bare store, not a CAS) → both run `reinit()`'s unlocked
`old=inst_` read → both `destroy_fn_(I0)` → double-free; plus `exchange`/`get_def`
read `inst_` unlocked vs the swap. Same root as **B1/B2**: `reinit` on a reentrant
(no-op `CallScope`) instance. **VERIFIED RESOLVED at e1b95b9:** the combo guard
(`set_on_fault`, commit 192791f) downgrades `on_fault=reinit → reuse` for any
reentrant (or staged-prepare) instance, and the machine-autoload path reaches it
via `make_adapter_guarded_` → `set_on_fault` (`xi_pm_load.hpp:93`+50), so
`reinit_pending` is never armed and `reinit()` never runs → the double-free is
unreachable. (The app-team fix — CAS-claim `reinit_pending` + widen the exclusive
gate around the whole read-swap-destroy — is the deeper fix IF the combo is ever
un-guarded to support reentrant+reinit for real; recorded, not needed now.)

### G2 — script `xi::Instance<T>` state plane unguarded vs a concurrent inspect · **P1 · CONFIRMED · UNFIXED**
`InstanceBase::set_def`/`get_def`/`exchange` (`xi_instance.hpp:63-69`) are plain
virtuals with NO locking; the inspect worker reads instance fields OUTSIDE `script_mu`
(`service_inspect.cpp:71-79,191-193`); `cmd:set_instance_def` (`service_cmd_project.cpp:
146-168`) does NOT quiesce dispatch. ASYMMETRY (verified): a BACKEND instance's
`set_def` is `CAbiInstanceAdapter::set_def` with a serializing `CallScope`
(`test_set_def_race`), but a SCRIPT `xi::Instance<T>` sits in the same registry with
NO adapter, NO CallScope, and the host adds none. A live operator re-tuning a
heavy-state instance (cv::Mat / string / vector / shared_ptr — the header's advertised
use) mid-stream reassigns a member while a worker reads it → **UAF** (scalar def → torn
read → wrong verdict, P2 floor). **NORMAL-REACHABLE** (live tuning is a standard
workflow; this is the SCRIPT-side escape of what round-2 F-refuted for backend
instances). Fix: quiesce dispatch around the script-instance `set_def`/`get_def`/
`exchange`/`prepare` + the load_project def-replay (like the lifecycle ops), or give
the script path a per-instance CallScope-equivalent the inspect deref also takes.

### G3 — `parallel_for` drops the inspect cancel-ticket → epoch-cancel voids a spared frame's worker iterations · **P2 · CONFIRMED · UNFIXED**
`parallel_for` (`xi_parallel.hpp:70-90`) captures owner + trigger-ctx per worker but
NOT `current_inspect_ticket_ref()` (async captures it, `xi_async.hpp:331/365`). OMP
workers keep ticket 0; `cancellation_requested()` treats 0 as "cancel me" when a cancel
is armed (`xi_async.hpp:226`) → the worker `continue`s and skips its whole static
`omp for` chunk → a watchdog-SPARED fresh frame (ticket ≥ cutoff) computes only the
master's fraction yet returns `inspect_ok=true` → silent wrong verdict in the ~1s
watchdog window. Normal (no-cancel) operation unaffected. Fix: capture `parent_ticket`
(+ the cancel token, a P3 sibling) and re-install per worker via a RAII scope, mirroring
async's `Scope`.

### G4 — `xi::kv()` cross-frame store races under `max_parallel>1` · **P2 · CONFIRMED (mechanism) · doc fix**
`xi::kv()` (`xi_kv.hpp:302`) is a process-global singleton `std::map`; the host does
not lock `kv_mutex` around inspect; under `dispatch_threads>1` two frames RMW the map
concurrently (UB on `std::map`). The header (`xi_kv.hpp:36-39`) tells authors to lock
only for `xi::async` and that "single-threaded scripts can ignore the mutex" — but a
script is single-threaded across frames only when `dispatch_threads==1`, so a
doc-correct kv-using script + the parallelism knob = an unlocked concurrent RMW.
**Reachable** by kv() + `max_parallel>1` (no exotic conditions). Fix: a host lock is the
wrong layer (kv() is called inline SDK-side, no host interception without serializing
the whole inspect) — the honest fix is the header/doc correction (`max_parallel>1` also
requires `kv_mutex`; drop the misleading "single-threaded scripts can ignore" line), or
make `Kv` internally synchronized so voluntary discipline isn't load-bearing.

### G5 — RT8 writer `stop()`/join not bounded against a slow-but-alive client · **P2 · CONFIRMED · UNFIXED · (on the RT8 writer)**
The writer's inner 1 MiB chunk loop (`xi_ws_server.hpp:703-716`) never re-checks
`writer_stop_`; `stop()` (`:347-366`) sets the flag + notify + join but does NOT
`::shutdown(client_)` before joining (shutdown is at `:360`, after). For a
slow-but-progressing client (each chunk < 1.5s, so SO_SNDTIMEO — which by the code's own
admission only catches a fully-wedged 0-drain peer — never fires), `join()` blocks for
`remaining_frame_bytes ÷ drain_rate` (a 16 MiB preview at 500 KB/s ≈ 30s), violating the
≤1.5s bound the header (`:344-346`) and doc 23 §Lifecycle promise. Not UAF/data-loss —
ungraceful teardown (a supervisor SIGKILL on timeout). Fix (one line, low-risk):
`stop()` does `::shutdown(client_, SHUT_RDWR)` BEFORE the join — the same unblock
`close_client` and the writer's error path already use — collapsing the worst-case join
to sub-second.

### Round-3b refuted-safe (app-team, condensed)
RT8 writer CORE solid (fd-reuse cross-connection send, epoch guard, single-FIFO order,
lost-wakeup on stop, byte-cap double-close, lock-order) · cap plane (rc-5 reentrancy
guard thread_local, quarantine never frees inst_, $v/$probe TOCTOU re-resolves under the
shared gate, recompile/rebuild/export all quiesce before FreeLibrary) · parallel
primitives (owner+trigger_ctx save/restore symmetric incl. rethrow path; worker
exceptions rethrown on the inspect thread) · hot-recompile (readers snapshot under
`script_mu` holding the module shared_ptr; `Param<T>` atomic scalar; thread_locals for
g_run_result/g_current_trigger/g_staged).

### Round-3b disposition
- **G1** — RESOLVED (combo guard, 192791f).
- **G2 / G3 / G4 / G5 — ALL FIXED @ `d05ed4a` (2026-07-05, full gate green):** G2 →
  `cmd_set_instance_def_` wrapped in `quiesce_dispatch_for_lifecycle_op_` (no worker
  mid-deref during a script-instance def swap). G3 → `parallel_for` captures the
  inspect ticket + cancel token and re-installs them per OMP worker (no more ticket-0
  false-cancel). G4 → the `xi::kv()` header thread-safety note corrected
  (`max_parallel>1` also requires `kv_mutex`; the "single-threaded scripts can ignore"
  line was false under the parallelism knob). G5 → RT8 writer `stop()` does
  `::shutdown(client_)` before `join()` (sub-second teardown vs a ~30s slow-client
  drain).
- **With this, EVERY finding across rounds 2–5 (F/RB/B/C/G/H/J) is FIXED, DEFUSED, or
  REFUTED. The red-team effort is closed.**

---

## Round-4 — process-lifecycle & fault-infrastructure sweep (2026-07-05, tree `78f4b0d`)

A third 4-way parallel pass, this time over the surfaces the data-flow rounds
never reached: the **FE supervisor / respawn path**, the **crash-dump / SEH /
watchdog** infrastructure under *concurrent* faults, the **metrics/observability**
snapshot readers, and the **image-codec + compress egress**. Each finding re-read
and hand-verified against the tree. **Status: findings only.** Two of the four
slices came back **clean** (codec, metrics); the defects cluster in the
**kill/respawn** and **concurrent-crash** paths — the parts that only run when
something is already going wrong, which is exactly why they were never stress-
tested.

Verdict spread: **3 P2 (one with P1 blast radius) · 5 P3.** No new P1s. The two
memory-unsafe P1s of this whole effort remain G1 (fixed) and G2 (unfixed).

> **Independent re-verification (2026-07-05, separate 4-way worktree pass @ tree
> `f822e35`): all of H1–H8 CONFIRMED — no refutations.** H2 is code-fact-confirmed
> but its reachability chains entirely off H1 (an abandoned BE#1 holding the log
> handle); fix H1 and H2 stops compounding. H1's four kill sites all discard the
> `WaitForSingleObject` return with no `WAIT_TIMEOUT` branch before `CloseHandle` +
> same-port/same-state-dir respawn; the shared job's `KILL_ON_JOB_CLOSE` fires only
> on FE exit, so it never reaps an abandoned-but-alive BE#1.
>
> **H1 FIXED @ `e72da07`:** `force_kill_be()` retries the terminate and returns true
> only once the handle is signaled (reading the real exit code then, not `STILL_ACTIVE`
> 259); on a budget miss the FE REFUSES to respawn — it latches the line down and
> exits so the job-object close reaps the abandoned backend.
>
> **ROUND 4 FULLY CLOSED — H2–H8 ALL FIXED @ `e744e8e` (2026-07-05, 5-way parallel
> implementation, full gate green):** H2 → a failed `be.log` open is now a spawn
> failure (the backend never runs against a non-truncated log, so a stale
> `autostart: ready` can't survive). H3 → `write_minidump` gated by a one-shot
> interlocked flag (only the first faulter touches dbghelp) + tid/ms in the filename
> stem. H4 → `reserve_fault_stack()` added to the OMP and async worker closures. H5 →
> atomic `consume_reinit_pending()` so one fault → exactly one rebuild+accounting
> (dispatch AND the cap-funnel sibling). H6 → job assigned at process creation via
> `PROC_THREAD_ATTRIBUTE_JOB_LIST` (no CreateProcess→assign orphan window). H7 → the
> three `_Exit` sites `await_dump()` (bounded) so a hard-exit can't truncate an
> in-flight minidump. H8 → the stats/health readers iterate a `mu_`-locked
> `snapshot_instances()`.

### H1 — FE unchecked kill-wait → abandoned backend + double-BE on one port/state dir · P2 (P1 blast radius) · CONFIRMED

**Where:** `backend/src/fe_main.cpp` — all four forced-kill sites use the same
pattern and **discard the wait result**: `:573-576` (autostart degraded),
`:582-585` (boot timeout), `:623-626` (heartbeat stale), `:646-649` (port
unresponsive).

```cpp
TerminateProcess(sp.pi.hProcess, 1);
WaitForSingleObject(sp.pi.hProcess, 5000);   // <-- return value discarded
GetExitCodeProcess(sp.pi.hProcess, &exit_code);
break;
```

`TerminateProcess` is **async** — the process object only signals once every
thread has terminated. A backend thread stuck in an uninterruptible kernel op
(blocking driver I/O, a page fault against a stalled mmap, an FS write to a
pressured disk — the RT8 heavy-load + record-to-disk scenario) can delay
signaling past 5 s. On `WAIT_TIMEOUT` the code doesn't check: it falls through,
`break`s, `CloseHandle`s its only handle to the still-alive BE#1 (`:655`), then
loops and spawns **BE#2 on the same WS port + `.xinsp_work` state dir**. Either
two backends are briefly co-resident (data-corruption / double-serve = **P1
impact**), or BE#2's bind fails → counted as a fresh failure → can latch the line
`RespawnLimitExceeded` "down" while the zombie BE#1 keeps running. The Job object
doesn't save it (`KILL_ON_JOB_CLOSE` fires only on FE exit, and both are in the
same job). Secondary: after a timeout `GetExitCodeProcess` returns
`STILL_ACTIVE (259)`, recorded as the death rc → forensic corruption.

**Fix:** check the wait result; on `WAIT_TIMEOUT` re-terminate / escalate and do
NOT respawn until the handle is signaled.

### H2 — stale `autostart: ready` marker → boot-readiness gate bypassed · P2 · CONFIRMED (mechanism), chains off H1

**Where:** `fe_main.cpp:564` (`log_contains(c.be_log, "autostart: ready")`) vs the
truncate-on-spawn at `:427` (`CreateFileA(..., CREATE_ALWAYS)`, share mode
`FILE_SHARE_READ` only).

The boot gate decides readiness by substring-scanning the whole `be.log`. The log
is truncated only by the spawn's `CreateFileA`. If that open **fails**
(`INVALID_HANDLE_VALUE` → `STARTF_USESTDHANDLES` never set, `:431-436`), the log
is neither truncated nor rewritten — it still holds the **previous** instance's
`autostart: ready` line, so the FE's next poll matches the stale marker the
instant BE#2's port opens and **skips the boot gate**; a genuinely boot-hanging
BE#2 is never caught. The open fails exactly when an abandoned BE#1 (H1) still
holds the log's write handle (share mode is read-only) → sharing violation. So H1
and H2 compound: one zombie backend both double-serves *and* blinds the boot gate.

**Fix:** gate readiness on a marker in a per-instance channel (or the heartbeat
file the new PID owns), not a reused shared log; treat a failed `be.log` open as a
spawn failure, not a continue-blind.

### H3 — concurrent unhandled faults race the minidump writer · P2 · CONFIRMED

**Where:** `backend/include/xi/xi_crash_dump.hpp:269` (`write_minidump`), installed
as the process-global `SetUnhandledExceptionFilter` with **no gate** (`:510`);
filename stem is pid + date + **HH MM SS only** (`:276-279`, no tid/ms);
`CreateFileA` uses `dwShareMode=0` (`:288`); `MiniDumpWriteDump` at `:313`.

The OS does **not** suspend sibling threads while one is in the unhandled-exception
filter, so under "multiple workers fault at once" two+ threads enter
`write_minidump` concurrently (reachable via faults that escape the per-inspect
SEH try: two unmanaged detached/plugin threads, two `std::terminate`s, two CRT
fastfail trips). Two consequences: **(a) dbghelp is not thread-safe** — concurrent
`MiniDumpWriteDump` calls are documented by MS to corrupt/hang; a hang means the
process never self-exits and the FE's coarse timeout has to kill it (delayed
respawn). **(b) filename collision** — two faults in the same wall-clock second
compute the same `.dmp`/`.json` path; the second `CreateFileA` (share=0) fails →
that thread writes no dump (lost forensics, possibly for the root-cause thread),
and thread A's `_Exit` can truncate B's in-progress dump. P2 not P1: the process
is already dying (no live-state UAF), the damage is lost/hung forensics.

**Fix:** gate `write_minidump` with a one-shot interlocked flag (first faulter
writes; later ones block — they're dying anyway) so only one thread touches
dbghelp; add tid+ms to the stem.

### H4 — OMP / async pool workers don't `reserve_fault_stack` (lane workers do) · P3 · CONFIRMED

**Where:** lane worker reserves (`service_dispatch.cpp:374`) and detached one-shot
reserves (`:525-526`), but the OMP worker (`xi_parallel.hpp:83`) and async worker
(`xi_async.hpp:354`) install only the SEH translator — no `reserve_fault_stack`.
The 128 KB guarantee exists (`xi_crash_dump.hpp:254-261`) so the filter has stack
to write a dump. A `STACK_OVERFLOW` that becomes **unhandled** on a pooled OMP/async
worker (overflow during unwinding, or a `noexcept` violation → terminate →
`write_minidump` on that worker) runs the dumper with only the default guard page →
no dump / secondary overflow. Low confidence on the escape (needs an overflow past
the closure's own catch), hence P3.

**Fix:** call `reserve_fault_stack()` at the top of the OMP/async worker closures,
matching the lane path.

### H5 — concurrent faults on one `on_fault=reinit` (non-reentrant) instance → redundant double-rebuild + spurious-quarantine window · P3 · CONFIRMED (NOT covered by the G1 guard)

**Where:** `service_sinks.cpp:111-113`/`:239-241` (the reinit-pending check runs
OUTSIDE any `CallScope`) → `apply_pending_reinit_` (`:72-78`) → `reinit()`
(`xi_cabi_adapter.hpp:537-583`, clears pending at entry THEN takes `CallScope`).

The G1 combo guard (`set_on_fault`, `xi_cabi_adapter.hpp:479`) downgrades
`on_fault=reinit` to `reuse` only for **reentrant OR prepare-exporting** instances
— so a **non-reentrant, non-prepare** instance keeps `reinit` live. For it
`effective_cap_()==1`, so `CallScope` IS engaged and two concurrent `reinit()`s
**serialize** (no double-free — this is why it's P3, not a G1 repeat). But the
check-then-act on `reinit_pending` isn't atomic with the rebuild: two faulting
callers both observe pending, both call `reinit()` → back-to-back rebuilds, the
second destroying the first's fresh `inst_` and building a third (redundant work
under fault load). Escalation-ordering window: if failing-rebuild A's
`note_reinit_fail` reaches the quarantine threshold before succeeding-rebuild B's
`reset_reinit_fails()`, a healthy (B-rebuilt) instance ends up quarantined.

**Fix:** fold the pending-check + rebuild + escalation under one per-instance
reinit lock so one fault → exactly one rebuild+accounting.

### H6 — `CREATE_SUSPENDED` → `AssignProcessToJobObject` window can orphan a suspended BE · P3 · CONFIRMED

**Where:** `fe_main.cpp:442-454` — `CreateProcess(CREATE_SUSPENDED)` → `Assign
ProcessToJobObject` → `ResumeThread`. If the FE dies between `CreateProcess` and
the job-assign, the child isn't in the job yet, so `KILL_ON_JOB_CLOSE` can't reap
it — it sits suspended forever holding the `be.log` handle. Narrow window, low
impact (it never runs → no double-serve), but a true orphan. Assign-before-resume
is otherwise the correct ordering.

### H7 — watchdog / drain hard-exit can truncate a concurrent minidump · P3 · CONFIRMED

**Where:** `std::_Exit` at `service_main.cpp:786` (watchdog hard trip),
`service_dispatch.cpp:601` (drain timeout), `service_inspect.cpp:220` (overflow
unrecoverable). If a worker is mid-`write_minidump` when the watchdog hard-trips or
drain times out, `_Exit` truncates the in-progress dump. Best-effort forensics
loss only (both events already mean "going down"); no live-state corruption.

### H8 — unlocked `project_.instances` iteration in the stats/health readers · P3 (latent, one-commit-away P1) · CONFIRMED

**Where:** `service_cmd_observability.cpp:246` (`cmd_image_pool_stats_`) and
`service_health.cpp:96` (`cmd_get_health_`) iterate `g_eng.plugin_mgr.project()
.instances` (an `unordered_map`) **without** `PluginManager::mu_`. Safe **today**
only because every structural mutator (create/remove/rename/open/close_instance)
and both readers run on the single WS poll thread and can't interleave. The
contrast is sharp: `instance_group()` (`xi_pm_instances.hpp:303-316`) reads the
same map and **does** take `mu_`, with a comment that unlocked reads race
`erase()`→UAF — because it runs on the source emit thread. The two stats readers
are the poll-thread cousins that never got the lock. The moment any off-poll-thread
instance mutation lands (a background auto-respawn, a worker-triggered
quarantine-remove), a concurrent `rehash`/`erase` turns both loops into
iterator-invalidation/UAF crashes.

**Fix (defensive):** take `mu_` for the loop, or add a `PluginManager` snapshot
helper, mirroring `instance_group`'s fix — cheap insurance before any off-poll
mutation lands.

### Hand-off (low-confidence, for the cap-plane owner)

`reinit()`'s `set_def_fn_(fresh)` (`xi_cabi_adapter.hpp:578-581`) runs under
`CallScope` but NOT under the exclusive `cap_gate_` (which covers only the
`destroy_fn_(old)`). For a **cap-providing, non-reentrant, non-prepare** instance
the G1 guard does not downgrade `reinit`, so `set_def(fresh)` could run concurrently
with a provider cap handler (which holds only the shared `cap_gate_`) on that fresh
instance. Not confirmed reachable — a non-reentrant cap provider is itself an odd
config (the funnel runs handlers with no `CallScope`). Flagged for the cap-plane
owner to judge against the G1 combo guard's coverage.

### Round-4 clean slices

- **Image codec + compress egress — CLEAN.** At v12 the host compress memo cache is
  DELETED (`service_main.cpp:638-655` is a pure `encode_via_capability` + memcpy,
  no static/map), so the doc-14 double-cache concern is refuted at the source. The
  one remaining cache (imgcodec plugin, content-hash keyed) copies the `shared_ptr`
  under its mutex on hit and both racers encode into their own buffer on miss
  (deterministic → byte-identical); `builder_add_bin`/`add_image` copy into a
  pack-owned buffer at add time (no ref into the map escapes; free-before-seal is
  safe). Only a P3-theoretical ~2⁻⁶⁴ hash collision (not load-induced).
- **Metrics / observability — CLEAN of P1/P2.** Counters are `atomic<uint64_t>`
  (no torn wire read); `source_emit_ages_us`/`stats_by_owner` copy under their
  mutex / use the WalkGuard deferred-reclaim handshake; lane high-watermark only
  grows under `lane->mu`. Residual P3s are by-design non-atomic multi-field
  snapshots (aggregate vs per-group depth sampled at different instants; `frames_ok
  + frames_error < frames_total` mid-update) — cosmetic, inherent to the lock-free
  counters. Plus the H8 latent-lock item above.

### Round-4 disposition & suggested order

All round-4 items are **P2/P3, none memory-unsafe under current control flow**
(H8 is latent). Suggested order:
1. **H1** — FE kill-wait check. Small, and its blast radius (double-BE / line-down)
   is the worst in this round; H2 stops compounding once H1 is fixed.
2. **H3** — one-shot gate + tid/ms on the minidump writer. Small; protects
   post-mortem quality on the exact multi-fault crash you most want to diagnose.
3. **H5** — per-instance reinit lock (also closes the redundant double-rebuild).
4. **H4 / H6 / H7 / H8** — one-liners / defensive guards, batch at leisure.
5. **Hand-off** — cap-plane owner confirms or dismisses against the G1 guard.

Nothing here blocks THE CUT.

---

## Round-5 — build/load/discovery & staged-egress sweep (2026-07-05, tree `78f4b0d`)

A fourth 4-way parallel pass over the remaining un-audited core surfaces: the
**WS receive framing + JSON command parse**, the **script-compile toolchain
subprocess**, the **project-load / plugin-discovery / version-identity** path,
and (after the metadata-doc plane came back deleted at v12) the **staged-egress
pack-handle lifetime**. Each finding re-read and hand-verified. **Status:
findings only.** Note: **documentation/comment defects are counted as findings
this round** (per maintainer direction) — a stale comment that re-derives a
deleted protocol is a real trap for the next reader.

Verdict spread: **3 P1 · 3 P2 (one a leak that becomes P1 under load) · 2 P3 · 1
doc-P3.** The WS RX slice came back essentially clean (one narrow P3). The P1s
cluster where they did in round 4: **lifecycle paths that tear down a live
DLL-backed adapter without quiescing** (the exact RT5 invariant — two more
escapees found) plus a **shared, non-namespaced build directory**.

> **Independent re-verification (2026-07-05, separate 4-way worktree pass @ tree
> `f822e35`): all of J1–J9 CONFIRMED — no refutations.** Two load-bearing
> corrections stand:
> - **J2 + J3 invalidate RT5's premise.** RT5 (doc 19 §C / [[21-redteam-load-findings]])
>   claimed `remove_instance` was *the ONLY* DLL-affecting lifecycle op without a
>   `quiesce_dispatch_for_lifecycle_op_` guard. It is not: `cmd_rescan_plugins_`
>   (J2, unconditional) and `cmd_create_instance_`'s machine-provider evict (J3,
>   autoload-gated) run the identical un-quiesced `~CAbiInstanceAdapter` /
>   `FreeLibrary` teardown. Both are one-line quiesce-wrap fixes.
> - **J1 is the hard reason concurrent QA is unsafe** — not just CPU-oversubscription
>   flakiness: every co-resident backend shares the fixed `temp/xinsp2/script_build`
>   + `.pch`, so parallel builds delete each other's `inspect_v0.dll` / tear the
>   shared PCH. `work_dir` must be pid/port-namespaced before QA can run parallel.
>
> **FIXED 2026-07-05 @ `f1fd54e` — the three P1s (J1, J2, J3):** J1 → `work_dir` is
> now per-PID (`temp/xinsp2/<pid>`, wiped on start for PID reuse; `runner_main` too),
> which also unblocks reliable parallel QA. J2 → `cmd_rescan_plugins_` wrapped in
> `quiesce_dispatch_for_lifecycle_op_` (also bounds J6). J3 → `cmd_create_instance_`
> wrapped in the same guard. Full gate green.
>
> **ROUND 5 FULLY CLOSED — J4–J9 ALL FIXED @ `e744e8e` (2026-07-05, full gate green):**
> J4 → off-dispatch-thread `push()` is rejected fail-loud (rc -6 + once-per-sink warn,
> mirroring the read-side off-thread guard) — no more leak / silent drop. J5 → the
> cl.exe compile wait is bounded (300 s ceiling + `TerminateProcess` on trip; the
> `_popen`/`std::system` fallbacks noted as residual). J6 → certify runs in an
> unlocked pre-pass that warms the on-disk verdict cache, so the locked scan no longer
> spawns a 30 s subprocess under `mu_`. J7 → prune keeps version N **and N-1** (the
> still-mapped previous script's PDB survives). J8 → the rx-accumulation cap is
> checked on the unparsed remainder (after the parse loop), not the raw buffer. J9 →
> the stale `meta_doc`/`DocRef` comments reworded to the single pack-handle reality.
>
> **J1 follow-up @ `638dc26`:** the per-PID dirs (a ~200 MB PCH each) accumulated
> unreaped and filled the disk (152 dirs / 27 GB → `C1085 No space left on device`);
> `reap_stale_build_dirs()` now sweeps dead-PID sibling dirs at startup, bounding
> usage to (live backends × PCH). H1 (round 4) also FIXED @ `e72da07`.

### J1 — script build uses a fixed, non-instance-namespaced `work_dir` → wrong/torn DLL load + corrupt PCH across co-resident backends · P1 · CONFIRMED

**Where:** `service_main.cpp:473` (`g_eng.work_dir = temp_directory_path()/"xinsp2"`
— fixed, not pid/port-qualified) → `service_cmd_lifecycle.cpp:125`
(`output_dir = work_dir/"script_build"`); the uniquifier is a **per-process**
counter that resets to 0 each start (`xi_script_compiler.hpp:659`
`static std::atomic<int> s_version{0}` → `inspect_v0.dll`, `:664`), with a
`remove(out_dll)` before build (`:668`) and a fixed-name PCH under `.pch`
(`:517-547`, sig keyed only on flags + backend-exe-mtime, not instance).

Two backends can be co-resident (the listen socket's `SO_EXCLUSIVEADDRUSE` only
blocks the *same* port; `--port` is configurable, and **the QA harness runs many
backends concurrently on `free_port()`** — doc 19 H3). All of them share
`temp/xinsp2/script_build` + `.pch`. Interleavings: **(a)** two backends both
compiling `inspect.cpp` target `inspect_v0.dll` — backend A's `remove(out_dll)`
can delete B's just-built DLL, or A `LoadLibrary`s a half-written / B's image
(wrong script logic in A's project). **(b)** any two concurrent script compiles
collide on the fixed-name `umbrella.{pch,obj}` — one `cl /Yc` writes it while the
other `/Yu` consumes it → corrupt/mismatched PCH → spurious `C1852/C2858` build
failure or a `cl.exe` crash. `inspect.obj` (`:789`, unversioned) is a second
same-stem collision point. No lock, no per-instance subdir, no pid suffix
anywhere. (Two `runner` processes collide the same way — `runner_main.cpp:396`
uses a separate but likewise fixed `xinsp2_runner_build`.)

**Fix:** pid/port-qualify `work_dir` (`temp/xinsp2/<pid>`), or put `script_build`
under the project's `.xinsp_work`; at minimum give the PCH dir a per-instance name.

### J2 — `cmd_rescan_plugins_` FreeLibrary's a LIVE plugin DLL out from under active instances · P1 · CONFIRMED

**Where:** `service_cmd_plugin.cpp:64-77` (`cmd_rescan_plugins_` calls
`scan_plugins` with **no** `quiesce_dispatch_for_lifecycle_op_` — unlike
recompile/rebuild/export/remove) → `xi_pm_discovery.hpp:155-159`
(`register_plugin_folder_locked_`, the "moved" branch):

```cpp
if (moved) {
    FreeLibrary(existing->second.handle);   // no live-instance check
    existing->second.handle = nullptr; ...
    plugins_[info.name] = std::move(info);
}
```

An open project has an instance backed by a global-dir plugin `foo` (a live
`CAbiInstanceAdapter` caching `dll_` = that `HMODULE` + `destroy_fn_`/process
pointers). A benign `cmd:rescan_plugins {"dir":"/other"}` where `/other` has a
plugin also named `foo` but a different folder (or a manifest `dll` edited after a
rebuild) makes `moved` true → **`FreeLibrary` on the still-referenced handle**.
The live adapter now holds pointers into unmapped code: the next dispatch call into
that instance, or `close_project`'s `~CAbiInstanceAdapter` → `destroy_fn_`, hits an
access violation / UAF. This is the RT5 class (un-quiesced teardown of a DLL-backed
runtime object) on the **discovery** path.

**Fix:** wrap `cmd_rescan_plugins_` in `quiesce_dispatch_for_lifecycle_op_`, and/or
refuse the FreeLibrary when a live instance references the handle.

### J3 — `cmd_create_instance_` evicts a live machine-autoload provider without quiescing · P1 (autoload-gated → P2 by reachability) · CONFIRMED

**Where:** `service_cmd_project.cpp:249-272` (`cmd_create_instance_` — no quiesce)
→ `xi_pm_instances.hpp:88` (`create_instance` → `evict_machine_provider_locked_`)
→ `xi_pm_load.hpp:62-72` (`InstanceRegistry::remove` + `machine_instances_.erase`
→ `shared_ptr` drop → `~CAbiInstanceAdapter` → `destroy_fn_(inst_)` + `sweep_caps_for`
+ `release_all_for` + `sweep_packs_for`).

With `--autoload-lib`, a lib plugin `cap.foo` is machine-provided and its cap
handlers are live in `CapRegistry` while dispatch runs. Creating a project
instance of `cap.foo` tears down the machine adapter — `xi_plugin_destroy` into
the DLL **plus three owner-sweeps** — with **dispatch live and not quiesced**. The
`shared_ptr` pin protects the adapter object during an in-flight cap call, but the
pack-registry owner-sweep racing a worker releasing an owner-tagged ref is the same
phantom-bucket window RT5 quiesced `remove_instance` for. RT5's claim that
`remove_instance` was "the ONLY [lifecycle op] that didn't quiesce" is now false.
P1 by class, gated on autoload-enabled + provider-machine-live (hence P2 by
reachability).

**Fix:** wrap `cmd_create_instance_` in `quiesce_dispatch_for_lifecycle_op_` like
its siblings.

### J4 — staged sink push from an off-dispatch thread leaks the pack ref (and silently drops the delivery) · P2 (P1 leak-to-exhaustion under load) · CONFIRMED

**Where:** `service_sinks.cpp:92` (`thread_local std::vector<StagedEmit> g_staged`)
+ `:154-158` (`use_push_pack_cb` retains then `g_staged.push_back` on the **calling**
thread) vs the drain/flush that only ever touch the **dispatch** thread's g_staged
(`drain_staged_emits_` `:494`, `flush_staged_emits_` `:510`, both bracketed by the
`StagedEmitGuard` over `run_one_inspection`, `service_inspect.cpp:341`).

`g_staged` is thread-local ("so parallel workers stage independently" — the comment
reveals the hole). A script that calls `xi::use("expose").push(pack)` from inside a
`xi::parallel_for`/`xi::async` body runs `use_push_pack_cb` on a **child** pool
thread `T_a ≠ T_d`: it `retain`s the pack (`:154`) and appends to **`T_a`'s**
`g_staged`. The dispatch thread `T_d` flushes/drains only **its own** g_staged, and
nothing ever drains `T_a`'s → the `+1` retain is never released (**pack leak**; with
OpenMP pools that reuse threads, `T_a`'s g_staged also accumulates across frames →
**leak-to-exhaustion**), and the push is **never delivered** (silent lost
actuation). `StagedEmit`/`TriggerEvent` has no destructor, so `T_a` dying doesn't
release it either. Manifests only at `max_parallel>1` (in the serial `#else`
fallback `xi::async` runs inline on the dispatch thread and flushes correctly —
which is why tests miss it). The codebase already guards the *read* sibling
(`current_trigger()` off-thread via `g_trigger_ctx_` + `warn_trigger_off_thread_`)
but has no analogue on the `push()` *write*.

**Fix:** reject `push()` off the trigger thread (mirror the `g_trigger_ctx_`
off-thread detection — fail-loud + once-per-name warn), or marshal off-thread staged
pushes back to the dispatch thread's g_staged under a lock.

### J5 — no timeout on any compile subprocess wait → a hung toolchain wedges the whole control plane · P2 · CONFIRMED

**Where:** `xi_script_compiler.hpp:500` (`run_with_env` →
`WaitForSingleObject(pi.hProcess, INFINITE)`), `:466` (`vcvars_env_block` blocking
`_popen` under a static mutex), `:850` (`std::system` fallback),
`xi_cmake_build.hpp:60` (`run_cmd_capture` `_popen`). All compile subprocess waits
are **unbounded** and run on the **poll thread** (compile is inline/synchronous).
A wedged child (`mspdbsrv.exe` contention, `link.exe` blocked on an AV-locked
output, a hung `vcvars`) freezes **all** WS commands with no cancel/stop path —
the compute-lane analog of RT8, but on the control plane, and not covered by any
watchdog on the toolchain path.

**Fix:** bounded `WaitForSingleObject` + `TerminateProcess` on trip; run compile
off the poll thread (or make it cancellable).

### J6 — `scan_plugins` holds `mu_` across the up-to-30 s certify subprocess wait → stalls `instance_group()` on the hot emit path · P2 · CONFIRMED

**Where:** `xi_pm_discovery.hpp:30` (`scan_plugins` takes `lock_guard(mu_)` for the
whole scan) → `:53` (`certify_folder_locked_` → `xi_certify.hpp:196-223`
`WaitForSingleObject(pi.hProcess, 30000)`), held under `mu_` per changed plugin,
vs `xi_pm_instances.hpp:308-316` (`instance_group` takes `mu_`, called on a
**source plugin's emit thread** for every trigger). A `cmd:rescan_plugins` during a
live run (un-quiesced, J2) can hold `mu_` up to 30 s per changed-hash plugin,
blocking the lane router on the emit/sink hot path → dropped frames / stalled
acquisition, plus every other PM entry (list/create/remove/to_json).

**Fix:** run certify outside `mu_` (snapshot the work list under the lock, certify
unlocked), or quiesce rescan (J2's fix helps here too).

### J7 — compile prune deletes the CURRENTLY-LIVE script's PDB before the swap · P3 · CONFIRMED

**Where:** `xi_script_compiler.hpp:878-904` — on a successful compile the prune
removes every `<stem>_v<N>.*` with `N != current`, running **inside `compile()`**,
i.e. **before** `load_script` + the `g_eng.script` swap
(`service_cmd_lifecycle.cpp:229,255`). At that instant the previous version is
still the live, mapped script; its `.dll` delete fails safely (mapped → sharing
violation, ignored) but its **`.pdb`/`.lib`/`.exp`/`.obj` do delete**. A crash in
the window (or a detached inspect still on the old module) then can't resolve the
old script's frames in the minidump. Crash-diagnosability loss only, narrow window,
no corruption/UAF.

### J8 — WS rx accumulation cap can false-drop a benign near-max frame pipelined with a following frame · P3 · CONFIRMED (low real-world reachability)

**Where:** `xi_ws_server.hpp:896-900` (`read_pending`) — the
`rx_buf_.size() > kMaxFrame + 16` cap is checked on the **raw accumulated buffer
before** the parse-loop (`:903-956`) parses+erases the completed frame.
`kMaxFrame+16` gives a single max frame only ~2 bytes of slack; if one `recv`
delivers the tail of a ~16 MiB frame *and* the head of a pipelined next frame
(same TCP segment, up to 16 KiB), `rx_buf_` momentarily exceeds the cap → a valid
client is closed (`:497-500`). Inbound frames are commands (small in practice —
pixels come via `frame_path`, not inline), so a ~16 MiB inbound frame is
near-unreachable for the real FE; hence P3.

**Fix:** apply the growth cap to the *unparsed remainder* (move the check after the
parse/erase loop), or raise it to `kMaxFrame + 16384 + 14`.

### J9 (doc) — stale comments re-derive the deleted metadata-doc protocol · P3 (doc) · CONFIRMED

**Where:** `service_internal.hpp:161` ("`xi::TriggerEvent rec; // images map +
meta_doc; host owns one ref to each`") and `:220` ("`// non-const: dtor reset()s
the event's DocRef`"). The metadata-doc plane (`DocRegistry`/`DocRef`/`DocChunkPool`)
was **hard-deleted at THE CUT (v12)** — `TriggerEvent` has neither an images map
nor a `meta_doc`/`DocRef`; it carries a single `xi_pack_handle`. These comments
will lead a future reader to re-derive a phantom doc-refcount protocol.

**Fix:** reword both to describe the pack handle.

### Round-5 clean slices

- **WS RX framing + JSON parse — essentially CLEAN.** 64-bit length is rejected
  `> kMaxFrame` before any alloc (no OOM); header bounds math guards each 125/126/127
  form; the fragmentation state machine resets on reconnect and caps `msg_buf_` at
  16 MiB (no torn command, no unbounded growth); RX does one `recv` per poll
  (select-gated → no poll-thread wedge from a dribbling client, unlike RT8 tx);
  `ParsedCmd.name`/`.args_json` are **owned** strings (no view into a buffer the next
  `recv` overwrites); parsers are iterative (no deep-nest stack overflow). Only J8.
- **Metadata-doc plane — DELETED (no code to race).** `xi_doc_registry.hpp` /
  `xi_doc_pool.hpp` are absent at v12; metadata now rides the pack plane or
  SDK-side `xi::kv()` (a single-mutex value-map, no pool/refcount). The only residue
  is the J9 comment-rot.

### Round-5 disposition & suggested order

1. **J2 / J3** — un-quiesced live-DLL teardown (rescan / create-instance). Memory-
   unsafe; both are a one-line `quiesce_dispatch_for_lifecycle_op_` wrap matching the
   sibling handlers. J2 is unconditional; J3 is autoload-gated.
2. **J1** — namespace `work_dir` by pid/port. Closes the wrong-DLL-load + corrupt-PCH
   for co-resident backends *and* runners; unblocks reliable concurrent QA.
3. **J4** — off-thread staged-push guard (mirror the existing off-thread read
   detection). Leak + silent lost actuation under `max_parallel>1`.
4. **J5 / J6** — bounded compile wait + certify-outside-`mu_`. Control-plane liveness.
5. **J7 / J8 / J9** — narrow / cosmetic / doc; batch at leisure.

Nothing here blocks THE CUT; J1–J3 are the memory-safety / correctness items and
each is a localized fix.

