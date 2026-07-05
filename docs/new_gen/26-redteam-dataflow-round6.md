# 26 — Red-team data-flow findings, rounds 6–7 (peripheral primitives, egress, + the rename P1)

> **Round 7 (P1 squeeze) — headline, added below.** A dedicated 4-way pass aimed
> *only* at memory-unsafety found **L1: `cmd_rename_instance_` tears down a live
> producer adapter without quiescing dispatch → UAF on a pack a still-running
> inspect holds (P1, CONFIRMED).** Same class as J2/J3/RT5 (un-quiesced live-adapter
> teardown); the last reachable escapee. Three of the four probes REFUTED their
> slice (pack refcount, dispatch custody, PM-map lifetime are all sound). Full
> writeup in the **Round 7** section at the end of this doc.

> **ALL ROUNDS-6/7 FINDINGS FIXED @ `02ad9e4`..`c3e7f1f` (2026-07-06, independently
> re-verified + full 8-stage gate green).** L1 (the P1) → one-line
> `quiesce_dispatch_for_lifecycle_op_` wrap on `cmd_rename_instance_`, closing the
> last un-quiesced live-adapter teardown (mirrors J2/J3); the `remove_instance`
> comment is now truthful (in-place rename IS guarded). L2 → `retain_untagged` on the
> push path. Round-6: K1 seq_cst walker slot loads (+ comment); K2 `sweep_packs_for`
> on the failed-ctor scope; K3 `mono_ms()` wall-time drain deadline; K4 `le`→
> `bucket_ms` wire key (+ schema/baseline/fixture/python/doc — additive, no version
> bump); K5/K6 metrics doc + `{}` init; K7 sample-clock-after-lock; K8 no-sink
> null-releaser fail-loud; K9 clear+toggle under one lock; K10-K12 + minor doc
> corrections. **The rounds-2..7 red-team effort is now genuinely closed.**

# 26 — Red-team data-flow findings, round 6 (peripheral primitives & egress)

**Threat model (unchanged from doc 25):** BENIGN plugins + BENIGN users. Only
stress / load / race defects — UAF, data race, torn / reordered / duplicated /
lost results, deadlock, lock-ordering inversion, leak, ABA, missed wakeup,
memory-ordering (acquire/release) error. **Documentation / comment defects also
count** (a comment asserting an invariant the code does not uphold, a wire key
whose name implies a semantics the code does not implement, stale post-CUT
residue). NOT in scope: malicious-input hardening, style, micro-perf.

**Method:** 4-way parallel audit over the surfaces rounds 2–5 had not yet swept —
the low-level sync primitives (`xi_clock` / `xi_owner_lock` / `xi_inflight_runs` /
`xi_thread`), trigger + graph capture (`xi_trigger_bus` / `xi_graph_capture`),
image custody + ordered emit (`xi_image_pool` / `xi_ingress` / `xi_emit_gate`),
and the observability egress (`xi_metrics` / `xi_status_sink` / `xi_binary_sink` /
`xi_cv`). Every finding below was personally re-verified against source.

**Headline:** no new P1. This round hits the diagnostic / observability / doc
strata — the core hot path (emit gate, ingress, Dekker handshakes, metrics
atomics) is **memory-safe and correct as written**. The findings are one
latent-UAF-on-weak-memory (benign on the x86 ship target but the comment
overclaims), one cold-path resource leak, two doc-vs-behavior mismatches that
mislead a monitor, and a cluster of comment/format defects. Severity legend as
doc 25: P1 memory-unsafe/data-loss · P2 correctness/observability under load ·
P3 latent/narrow/diagnostic/doc.

---

## Findings (K-series)

### K1 — `image_pool`: walker slot load is `acquire`, not `seq_cst` — deferred-reclaim handshake comment overclaims its memory-model guarantee
`backend/include/xi/xi_image_pool.hpp:321, 369, 388` (walker loads) vs `:327`
(releaser seq_cst null-store), `:832` (WalkGuard fetch_add seq_cst), `:845`
(reclaim_entry_ load seq_cst); comment `:814-822`.
**Severity: P3 latent (would be P1 UAF on a weakly-ordered target).**

The deferred-reclamation StoreLoad handshake rests on a Dekker mutual-exclusion:
release nulls the slot (seq_cst) **before** `reclaim_entry_` loads
`active_walkers_` (seq_cst); a walker bumps `active_walkers_` (seq_cst) **before**
loading the slot. The comment claims *"the seq_cst total order then guarantees:
if a walker observed the entry … the releaser observes `active_walkers_ > 0` and
defers."*

Three of the four ops are seq_cst — but the fourth, the walker's per-slot
`slots_[i].entry.load(std::memory_order_acquire)`, is only **acquire**, so it is
**not in the seq_cst total order** `S`. The total-order argument therefore does
not close:

- Releaser reads `active_walkers_ == 0` ⟹ its load `L_walk` precedes the walker's
  increment `W` in `S`, and (sequenced-before) the null-store `S_null <_S L_walk <_S W`.
- Walker: `W` is sequenced-before its `acquire` load `L_entry`. But `S_null <_S W`
  between seq_cst ops on *different reasoning chains* does **not** establish
  `S_null` *happens-before* `L_entry` — an acquire load only synchronizes with the
  release store it actually **reads from**, and here `L_entry` reads the earlier
  *non-null create* value, not `S_null`.

So the interleaving "walker reads non-null **and** releaser reads
`active_walkers_ == 0`" is formally admissible: releaser `delete e` inline
(`:846`) while the walker dereferences `e->pixels.size()` (`:373/394`) → **UAF
read**. On x86-TSO this cannot occur (the seq_cst null-store lowers to
`xchg`/`mov+mfence` draining the store buffer, and the walker's `fetch_add` is a
`lock`-prefixed full barrier, so a plain acquire load can't hoist ahead) — hence
**benign on the shipping MSVC/x86 target**. But Windows-on-ARM64 is a real weak
target, and the comment asserts an architecture-independent guarantee the code
does not provide. **Fix:** make the three walker slot loads `seq_cst` (same
location as the null-store, so coherence then forces them to observe the null),
or drop a `std::atomic_thread_fence(seq_cst)` right after the `WalkGuard`
increment.

### K2 — `image_pool`: `ImagePoolOwnerScope` failure-cleanup sweeps images + caps but **not packs** → leaked pack ref on failed construction
`backend/include/xi/xi_image_pool.hpp:926-934` vs the three-plane teardown
contract at `:585-589` and the adapter dtor `xi_cabi_adapter.hpp:309`
(`sweep_caps_for`) + `:334` (`sweep_packs_for`).
**Severity: P3 (cold-path resource leak).**

The owner-scope RAII exists so a *failed* instance construction reclaims every
owner-tagged resource the factory touched. The success path hands `id_` to the
adapter, whose dtor sweeps all three planes (images `release_all_for`, caps
`sweep_caps_for`, packs `sweep_packs_for`). The failure path — `~ImagePoolOwnerScope`
— calls only:

```
ImagePool::instance().release_all_for(id_);   // images
ImagePool::sweep_caps_for(id_);               // caps
```

It never calls `ImagePool::sweep_packs_for(id_)`. A factory that seals a pack into
the PackRegistry under its owner id and *then* throws / fails a later
construction step (exactly the scenario this scope cleans up) leaks that pack ref
for the process lifetime — whereas an identically-tagged image or cap
registration *is* reclaimed. The asymmetry with the `sweep_caps_for` call sitting
one line below is the tell. **Fix:** add `ImagePool::sweep_packs_for(id_);` to the
dtor, matching the adapter's three-plane sweep.

### K3 — `inflight_runs`: `drain()` cap counts 1 ms-sleep **iterations** as if wall-ms — Windows timer granularity stretches the documented "~50 s" bound ~10×
`backend/include/xi/xi_inflight_runs.hpp:104-108`; comment `:102`
(“capped (default ~50 s) so a wedged inspect can't hang process exit”).
**Severity: P2 (doc + teardown behavior).**

```
bool drain(int cap_ms = 50000) {
    for (int i = 0; count_.load() != 0 && i < cap_ms; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return count_.load() == 0;
}
```

The loop bounds **iterations**, assuming each `sleep_for(1ms)` costs ~1 ms. On
Windows at the default scheduler timer resolution (~15.6 ms) a 1 ms sleep rounds
up to a full quantum, so 50000 iterations can take **up to ~13 minutes** of wall
time, not ~50 s. `controlled_shutdown_teardown_()` waits on `drain()` before its
safety hard-exit, so when an inspect is genuinely wedged, process teardown (and
the FE-observed respawn) can hang roughly an order of magnitude longer than the
documented safety bound. **Fix:** measure elapsed wall time via `mono_ms()`
against a deadline rather than counting iterations (or raise the timer resolution
for the drain window).

### K4 — `metrics`: histogram buckets are labeled `"le"` (Prometheus's **cumulative** convention) but implemented **non-cumulative** → a standard consumer misreads every quantile
`backend/include/xi/xi_metrics.hpp:80, 102` (wire key) vs `:24-26` (header:
"buckets are cumulative-free") + `:69-73` (each bucket counts only
`[prev_bound, this_bound)`).
**Severity: P2 (doc/format — wrong data at the consumer).**

`le` is the near-universal Prometheus label for *cumulative* buckets (count of all
observations ≤ edge). The exporter emits `{"le":<edge>,"count":n}` but each `count`
is the **non-cumulative** occupancy of that single bucket. A monitor written to the
`le` convention (the obvious reading of the key) treats the counts as cumulative,
producing monotonic-nonsense deltas and garbage p-quantile estimates — precisely on
the `cmd:metrics` export path this histogram exists to feed. The wire key asserts a
semantics the code does not implement. **Fix:** rename the key (`lt` / `upper` /
`bucket_ms`), or make the buckets actually cumulative to match `le`.

### K5 — `metrics`: `snapshot_json` is not a consistent cut — exported buckets can sum to less than `count` under live dispatch
`backend/include/xi/xi_metrics.hpp:88-107` (`total` loaded `:88`, buckets `:103`)
vs `record_frame` increment order `:60` (total first) … `:73` (bucket last).
**Severity: P3 (observability skew, self-healing).**

Each counter is an independent `relaxed` atomic loaded at a different instant.
`record_frame` bumps `frames_total_` first (`:60`) and the bucket last (`:73`); a
worker preempted between them lets the poll thread read `total = N` with bucket
atomics reflecting `N-1`. The emitted JSON then has `Σbuckets < count` and
`frames_ok + frames_error < frames_total`. The skew is bounded by the number of
frames mid-`record_frame` (nanoseconds of work each), always in the "total runs
ahead" direction, and self-corrects on the next snapshot — so it's a narrow
observability wart, not data loss. The `test_metrics.cpp` "buckets partition every
frame" invariant only holds quiescent (post-`join()`), never on the live export
path. Note in the header that the snapshot is a non-atomic multi-counter read.

### K6 — `metrics`: `buckets_[]` array has no member initializer, unlike every scalar sibling
`backend/include/xi/xi_metrics.hpp:140` (`std::atomic<uint64_t> buckets_[kBuckets+1];`,
no `{}`) vs `:136-139` (each scalar counter `{0}`).
**Severity: P3 (latent).**

Pre-C++20 `std::atomic`'s default constructor does **not** zero the value. Safe
today *only* because the sole instance is the function-local `static` in
`instance()` (`:51`), so static zero-initialization runs before the trivial ctor.
If `MetricsRegistry` is ever stack/heap-constructed (a test double, a future
per-session registry), the buckets start indeterminate and the first `fetch_add`
accumulates onto garbage → corrupt histogram, no crash. Inconsistent with the
siblings; add `{}`.

### K7 — `trigger_bus`: `source_emit_ages_us()` samples the clock **before** taking the lock → negative age for the healthiest source; inconsistent with its sibling
`backend/include/xi/xi_trigger_bus.hpp:210-217` (`now` sampled `:212`, before
`mu_` `:213`) vs the correct sibling `last_emit_age_us()` `:204-207` (samples the
clock *after* the atomic load).
**Severity: P3 (observability).**

Thread T1 enters `source_emit_ages_us`, samples `now`, then blocks on `mu_`.
Thread T2 runs `emit_pack`, takes `mu_`, stamps
`source_last_emit_mono_us_[src] = mono` with `mono > now`. T1 acquires the lock and
computes `now - t < 0` for that source. A monitor that thresholds "age > X = stalled"
or treats the value as unsigned gets a wild reading for the *most recently emitting*
(healthiest) source. **Fix:** sample `now` after the `lock_guard`, matching
`last_emit_age_us`.

### K8 — `trigger_bus`: no-sink `emit_pack` path drops the pack ref via a releaser that no-ops when unset — comment claims an install-ordering invariant the code doesn't enforce
`backend/include/xi/xi_trigger_bus.hpp:158-162` (no-sink `release_pack_(pack)`) +
`:174-180` (`release_pack_` is a no-op while `pack_releaser_` is null); comment
`:168-169` ("Null until installed — a bus with no pack plane simply never sees a
pack event").
**Severity: P3 (doc + latent startup-ordering leak).**

`emit_pack` unconditionally consumes the caller's pack ref; on the no-sink branch
it routes to `release_pack_`, which silently does nothing while the releaser is
still null. The comment asserts a bus with no pack plane never sees a pack event —
but nothing ties pack-ingress wiring to `set_pack_releaser` having run. If the host
wires `emit_pack` before the releaser (boot ordering), a real pack emitted while
(a) no sink is installed and (b) the releaser is null has its retained ref
dropped-to-nowhere → host `PackRegistry` refcount never decremented. **Fix:** either
assert `pack_releaser_ != null` when a non-null pack arrives, or soften the comment
to state the actual (weaker) guarantee.

### K9 — `graph_capture`: `set()` clears the buffer under the lock but flips `on_` **outside** it → stale append into the just-cleared buffer on disable
`backend/include/xi/xi_graph_capture.hpp:48-51`.
**Severity: P3 (diagnostic correctness).**

```
void set(bool on) {
    { std::lock_guard<std::mutex> lk(mu_); calls_.clear(); }   // 49
    on_.store(on, std::memory_order_relaxed);                  // 50
}
```

`record()` gates on the unlocked relaxed `enabled()`. On `set(false)`: a worker
reads `enabled() == true` before line 50, proceeds, and by the time it grabs `mu_`
in `record()` the `clear()` has already run — it appends a stale call into the
just-cleared buffer, so after `set(false)` returns `calls_` is non-empty with
leftovers from the disable window. Not memory-unsafe (all `calls_` access is
locked), but the "enabling clears any prior recording" arm/clear guarantee is racy
under concurrent record/toggle. **Fix:** fold the `on_.store` inside the same lock
scope as `clear()`.

### K10 — `graph_capture`: ring-eviction comment ("still reconstructs the full graph") overstates what a post-eviction snapshot guarantees
`backend/include/xi/xi_graph_capture.hpp:76-78, 130-131` (comment) vs the forward
producer-map reconstruction `:99-113`.
**Severity: P3 (doc).**

`record()` rings the buffer past `kMaxCalls` (`:78`); `snapshot()` builds the
producer map by forward iteration over *retained* calls only. When the ring cut
lands mid-frame the earliest retained frame is partial: a consumer whose producer
call was evicted finds no `producer[h]` entry, so that A→B edge is missing until a
later complete frame reproduces it. The comment's "reconstructs the full graph" is
true only in steady state across ≥2 full frames; a snapshot taken right after
eviction (or of a topology whose period approaches `kMaxCalls`) under-reports edges.

### K11 — `owner_lock`: `pid_alive()` resolves the "live process, other user" ambiguity in **opposite** directions on Windows vs Linux; only Windows matches the prose
`backend/include/xi/xi_owner_lock.hpp:48-60`.
**Severity: P3 (doc / cross-platform behavior).**

The comment says the check leans toward "alive only on positive evidence, so a
stale stamp is reclaimed rather than a dead pid blocking forever." The Windows
branch treats *any* `OpenProcess` failure — including `ERROR_ACCESS_DENIED` on a
**live** process owned by another user — as dead → reclaim. The Linux branch does
the opposite: `EPERM` (live, other user) → **alive**. The two platforms resolve the
identical ambiguity in opposite directions and only Windows matches the "reclaim on
ambiguity" prose. Advisory-only (worst case: a spurious warning or spurious silent
takeover, never data loss), but a documented-invariant / code mismatch across
platforms.

### K12 — `owner_lock`: pid-reuse ABA unacknowledged; the stamp's `ts_ms` is never used to disambiguate reuse
`backend/include/xi/xi_owner_lock.hpp` (`pid_alive` / `read`, `ts_ms` at ~`:62/68`).
**Severity: P3 (latent / doc).**

Classic pid ABA: the owning backend crashes, the OS recycles its pid for an
unrelated process, and `pid_alive(old_pid)` returns true → `read()` judges the stamp
"held by a different live process" and surfaces a false "project already open
elsewhere" warning. The stamp carries a `ts_ms` that is never used to disambiguate
reuse, and the header does not acknowledge pid recycling. Advisory only (warn, never
refuse) → no data loss; reporting as a doc/latent gap.

### Minor doc notes (batch at leisure)
- **`image_pool:719-726`** — `door_matches_fields` comment describes an
  `emit_record` freeze-check that no longer exists (the emit block `:748-751`
  checks only `emit_binary`, itself annotated "[ABI v12 — emit_record dropped at
  THE CUT]"). Stale post-CUT residue. (P3 doc.)
- **`image_pool:3`** — top-of-file title "host-side refcounted image pool
  (lock-free)" is overstated: the last-ref `release()` routes through
  `reclaim_entry_`, which takes `retire_mu_` (`:849-852`) whenever a diagnostic
  walk is in flight. Line 7's "most … are lock-free" is accurate; the unqualified
  parenthetical on line 3 is not. (P3 doc.)
- **`metrics:57-58`** — "independent counters … no happens-before is published"
  understates the *value* invariant (`total == ok+error == Σbuckets`) the exporter
  and tests rely on; the snapshot is a non-atomic, mutually-inconsistent read
  (see K5). (P3 doc.)
- **`status_sink:17-20` / `binary_sink:15-18`** — the sink function-pointer holders
  are plain non-atomic statics; `service_main.cpp:642-643` asserts `send_binary` is
  thread-safe, but that serialization lives in the WS server, not the sink layer.
  Benign under the boot-once install discipline; would be a data race if any sink
  were ever re-installed at runtime. (P3 doc.)
- **`inflight_runs` drain during `begin_shutdown`** — a source that keeps attempting
  launches concurrently with `drain()` does bump-then-bail (transient `count_==1`),
  so a 1 ms sample can repeatedly observe nonzero and mis-report "wedged in-flight
  inspect(s)" on the teardown diagnostic. Very narrow — real callers `clear_sink()`
  before draining, stopping the launch stream — but the primitive alone does not
  preclude it. (P3 narrow.)

---

## Clean (verified, no defect)

- **`xi_emit_gate.hpp` (EmitTurn / EmitGate)** — turn counter integrity holds:
  `next` advances by exactly 1 per `complete()`, only when `g_->next == seq_`, so no
  duplicate (one seq matches `next`) and no skip (contiguous seqs each complete once;
  the dtor backstop guarantees completion on any early-return/exception).
  `wait_turn()` idempotency + stop-wake are consistent — a stop flips `keep_going`,
  wakes all parked seqs, none advance the cursor, late/never-parked seqs
  short-circuit on the predicate → no stranded waiter, no deadlock, no out-of-order
  emit. Comments accurate.
- **`xi_ingress.hpp`** — fully stateless/pure per call; `canonicalize_entry` copies
  `opts.ext_policy` into a local before the strip (`:93-96`), so concurrent calls
  sharing one `Options&` are race-free. No shared mutable state, no custody of its
  own.
- **`xi_inflight_runs.hpp` Dekker handshake** — CORRECT. `launch()` bumps `count_`
  (CAS) then reads `shutting_`/`paused_`; `begin_shutdown()`/`pause()` set the flag
  then `drain()` reads `count_`. All seq_cst → the single total order guarantees a
  launch racing teardown is either counted by the drain or bails. Cap CAS loop is
  ABA-immune (numeric counter). (Only the drain *cap* — K3 — and the spurious-nonzero
  diagnostic are notes.)
- **`xi_clock.hpp`** — pure `std::chrono` wrappers, no shared state; wall-vs-mono
  split ("never subtract wall from mono") correctly documented and implemented.
- **`xi_thread.hpp`** — `spawn_worker` moves callable+args into the closure, installs
  the SEH translator inside the thread body before invoking, swallows/logs escapes;
  adds no shared mutable state.
- **`xi_cv.hpp`** — despite the name, OpenCV Mat-adapter helpers, not
  condition-variable primitives; concurrency-neutral, the non-owning-Mat-lifetime
  contract is correctly documented as a caller responsibility.
- **`xi_metrics.hpp` atomics** — all counters correctly lock-free; no torn 64-bit
  reads, no true data race (K5 is a snapshot-consistency wart, K6 a latent-init
  note, K4 a wire-key mislabel).
- **`trigger_bus` single-sink model** — `sink_` copied under `mu_` then fired outside
  the lock; no iterator/subscriber-mutation race, no re-entrant self-deadlock.
  `pack_releaser_` uses release/acquire correctly; `reset()` guards the map.

---

## Round-6 disposition & suggested order

1. **K2** — add the missing `sweep_packs_for` to `~ImagePoolOwnerScope`. A real
   (cold-path) leak; one line, symmetric with the adapter dtor.
2. **K4** — rename the metrics `"le"` key (or make buckets cumulative). Wrong data
   at every standard consumer; a wire-contract fix, best done before more consumers
   bind to it.
3. **K3** — make `drain()` deadline-based (`mono_ms`) so the documented safety bound
   is real. Matters only when an inspect is wedged, but that's exactly when the bound
   is load-bearing.
4. **K1** — seq_cst the three walker slot loads (or fence after the WalkGuard bump).
   Benign on x86 today; do it before any Windows-on-ARM64 target, and fix the
   overclaiming comment regardless.
5. **K7 / K9** — one-line lock-scope corrections (sample-clock-after-lock;
   clear+toggle under one lock).
6. **K5 / K6 / K8 / K10 / K11 / K12 + minor notes** — doc/format/latent; batch at
   leisure.

No P1 this round. Nothing here blocks THE CUT. K1 is the only memory-safety item and
it is benign on the shipping target; K2 is the only real (cold-path) leak. The core
data-flow hot path remains sound.

---
---

# Round 7 — P1 squeeze (cross-thread custody lifetime)

**Goal:** unlike round 6 (which swept diagnostic/observability strata), this pass
targeted **only P1** — UAF / double-free / data-loss on a live object under
STRESS/LOAD/RACE — across the four cross-thread custody handoffs: pack-plane
refcount, cabi-adapter lifecycle, dispatch custody, and PM instance-map lifetime.
Each probe was required to hand back an exact free-site + use-site interleaving or
mark its slice REFUTED. Result: **1 CONFIRMED P1 (L1), 3 slices REFUTED clean, 1
P2/P3 ledger wart (L2).**

## L1 — `cmd_rename_instance_` destroys a live producer adapter WITHOUT quiescing dispatch → UAF on a pack a still-running inspect holds — **P1, CONFIRMED**

**Class:** the RT5 / J2 / J3 family — a lifecycle op that tears down a live
DLL-backed adapter must quiesce dispatch first. `rename_instance` is the last
reachable escapee. Every sibling (`create_instance`, `remove_instance`,
`commit_group`, recompile / rebuild / close / open) wraps in
`quiesce_dispatch_for_lifecycle_op_`; **`cmd_rename_instance_` does not.**

### Sites
- **No guard:** `backend/src/service_cmd_project.cpp:317-322` — `cmd_rename_instance_`
  calls `plugin_mgr.rename_instance(...)` with no `quiesce_…` guard (contrast
  `cmd_remove_instance_:304`).
- **Destroy:** `backend/include/xi/xi_pm_instances.hpp:262` `InstanceRegistry::remove(old_name)`
  + `:283` `project_.instances.erase(old_name)` → last `shared_ptr` drops →
  `~CAbiInstanceAdapter`.
- **Free:** `backend/include/xi/xi_cabi_adapter.hpp:334` `ImagePool::sweep_packs_for(owner_id_)`
  (and `:324` `release_all_for`, `:299` `destroy_fn_(inst_)`).
- **Use:** the lane worker still inside inspect frame N, dereferencing the
  `ScriptPack r` it got from `use("<renamed>").process(in)` — any
  `xi_use.hpp:262` `get_image` / a forward `use("next").process(r)` / an emit.

### Why the pack is charged to the producer and is single-owner (the make-or-break)
The door's output is minted **owner-tagged to the producing instance**:
`run_pack_door` wraps the door in `ImagePool::OwnerGuard og(owner_id_)`
(`xi_cabi_adapter.hpp:414`), so `PackRegistry::seal` charges the creator's `rc=1`
ref to `owner_id_` (`xi_pack_abi.hpp:125`, `current_owner()`). That ref is then
**transferred by move, not re-retained**: `use_pack_process_cb` returns the handle
as-is (`service_sinks.cpp:271`, no retain) and `UseProxy::process` adopts it into an
owning `ScriptPack` whose keepalive only *releases* (`xi_use.hpp:855-858`, no
`retain`). So the script's `r` holds a pack whose **only** ledger charge is the
producer's — the "never free while another owner's bucket is non-empty" fail-safe
(`xi_pack_abi.hpp:194-208`) does **not** spare it: `sweep_packs_for(producer_owner)`
takes `k=1`, `remaining=0`, `reclaim = rc − remaining = 1`, `rc→0`, `owners.empty()`
→ **pack destroyed** (and its pool image handles released).

### The interleaving (continuous dispatch, `max_parallel>1`, benign HMI rename)
1. Lane worker **W**, inspect frame N: script runs
   `auto r = xi::use("binarize").process(in);`. Inside, `find("binarize")` pins the
   adapter **only for that call**, the door seals its output (rc=1, charged
   `owner_B`), `process()` returns → **the adapter pin drops**. W keeps inspecting,
   holding `r` (the sole, producer-charged ref).
2. Poll thread: `cmd:rename_instance {name:"binarize", new_name:"binarize2"}`. **No
   quiesce.** `rename_instance` builds the new adapter (`owner_B2`), then
   `InstanceRegistry::remove("binarize")` + `instances.erase("binarize")` → old
   adapter's last ref drops → `~CAbiInstanceAdapter` → `sweep_packs_for(owner_B)`
   → **frees `r`'s pack + its pool images**.
3. W, still in frame N, touches `r` → `get_image` / forward / emit → **UAF read** of
   a freed `Pack` and its generation-recycled `ImagePool` slots (access violation, or
   a torn cross-frame read once the slot is re-handed to a new frame).

### Precision note (correcting an over-statement worth recording)
The trailing `ScriptPack` keepalive `fi->release(out)` at inspect end is **not** a
double-free: pack handles come from a monotonic `next_` and are **never reused**
(`xi_pack_abi.hpp:118`), so `release_as` on the already-erased handle is a null-safe
no-op (`:162-163`). The registry `rc` is fully serialized under `mu_`, so there is no
second decrement of a live slot either. The solid, reachable defect is the **UAF read
in step 3** — memory-unsafe, benign-reachable via a normal HMI rename while the line
runs. That is sufficient for P1.

### Why the siblings are safe and this is not
`quiesce_dispatch_for_lifecycle_op_` stops + joins the dispatch pool and pauses/drains
detached in-flight runs **before** the adapter is destroyed, so no `ScriptPack`
holding a producer-charged ref is live at teardown — the sweep then reclaims only
genuine leaks. The `cmd_remove_instance_` comment
(`service_cmd_project.cpp:294-303`) names exactly this hazard and even lists
"rename-**via-reload**" as covered — but the reachable command path
(`cmd_rename_instance_` → the *in-place* `PM::rename_instance` factory-recreate) is
**not** the reload path and carries no guard. So there is also a **doc defect**: that
comment asserts rename is covered when the in-place rename is not.

### Fix
One line, identical to the siblings — before calling `rename_instance`:
```cpp
auto _rn_guard = quiesce_dispatch_for_lifecycle_op_("rename_instance", &srv);
```
**Deeper (optional, the latent root the quiesce masks):** a door-output pack is
transferred across the ABI seam still charged to the *producer* owner. Re-tagging it
off the producer (untagged, or retagged to the consumer/script owner) at the transfer
point would make it structurally immune to a producer sweep — the same asymmetry L2
flags on the push path. The one-line quiesce closes the reachable P1; the retag is the
principled fix.

## L2 — pack push retains owner-tagged but releases untagged → phantom ledger bucket (P2/P3, fail-closed)
`backend/src/service_sinks.cpp:179` (`PackRegistry::instance().retain(pack)` →
`retain_as(pack, current_owner())`, and at push time `current_owner()` is the script
owner S via the `OwnerGuard` at `service_inspect.cpp:153`) vs the balancing release in
`flush_staged_emits_` → `release_trigger_event_` (`release_as(pack, 0)`, off-guard).
`ledger_release(owner=0)` is deliberately unattributable (`xi_pack_abi.hpp:266`), so it
decrements `rc` but not bucket S → a **phantom bucket S** persists, briefly violating
the documented `Σbuckets ≤ rc` invariant (`:176-178`).

**Not a P1 — verified fail-closed.** Worked both sweep directions on the worst case
`rc=1, {Y:1, S:1-phantom}`: `sweep(S)` → `reclaim = rc − remaining = 1 − 1 = 0`;
`sweep(Y)` → `1 − 1 = 0`. The `reclaim = rc − remaining` formula (remaining = sum of
*surviving* buckets after `ledger_take`) protects every real co-owner **regardless of**
the invariant violation, so the phantom can never cause an over-release; it is
discarded harmlessly at S's own teardown (which is deferred past the whole inspect by
the `LoadedScript` `module_lifetime` pin, so no concurrent sweep even observes the
transient). Impact is limited to a ledger/`owner_refs` diagnostic over-count. **Fix:**
`retain_untagged` at `service_sinks.cpp:179`, matching `f_emit_pack`'s deliberate use
of it for the identical off-thread-release reason.

## REFUTED slices (traced, no P1 — recorded so the next round doesn't re-walk them)
- **Pack-plane refcount / door forwarding — REFUTED.** One handle traced mint → seal
  → untagged event ref → worker scope → `release_as` last-ref free. The dispatch event
  ref is **untagged** (`f_emit_pack` uses `retain_untagged`), so `release_all_for`
  can't reclaim it; handles are monotonic/never-reused (no ABA); registry `rc` is fully
  `mu_`-serialized (no lock-free race); the script-owner sweep is deferred past the
  inspect by the `LoadedScript` copy. Only wart is L2.
- **Dispatch custody handoff — REFUTED.** `TriggerEvent` has no dtor + trivial handle,
  so release is manual and single-sited (`release_trigger_event_`, idempotent). Drop
  path touches only `q.front()` (disjoint from a worker's popped event); lane teardown
  captures `lane` by `shared_ptr` and **joins** workers before draining `q`; staged
  flush moves-out-then-releases; `continuous`-toggle routes each event to exactly one
  owner (F2 window now `account_dropped_frame_`). One-owner-per-ref holds on every
  branch.
- **PM instance-map lifetime — REFUTED.** The map stores `shared_ptr<InstanceBase>`;
  every cross-thread consumer copies it out **under `mu_`** before deref
  (`InstanceRegistry::find` `xi_instance.hpp:103-107`; `instance_group` returns a
  `std::string` by value; cap funnel pins + `cap_reinit_gate`). Erases only drop
  map/registry refs — the worker's own `shared_ptr` keeps the adapter alive for the
  *`process()` call*. **Caveat that L1 exploits:** that pin covers the call, not the
  whole inspect — the *pack* outlives the call and is what L1 frees. The adapter-object
  UAF is refuted; the pack UAF is L1.
- **cabi-adapter `set_def` / reinit / prepare / cap-funnel — REFUTED.** `set_def` runs
  under `CallScope` and frees nothing; `reinit` is unreachable for the
  reentrant/prepare class (the `set_on_fault` downgrade), `cap_gate_` covers the funnel,
  and the ungated `prepare()` is exactly the combo the load-time guard defuses. No
  lifetime gap **except** the un-quiesced rename teardown (L1).

## Round-7 disposition
1. **L1 (P1)** — add the one-line `quiesce_dispatch_for_lifecycle_op_` wrap to
   `cmd_rename_instance_`. Closes the last un-quiesced live-adapter teardown; mirrors
   the fix already shipped for J2/J3. Highest priority — memory-unsafe, benign-reachable.
2. **L2 (P2/P3)** — `retain_untagged` on the push path; restores the ledger invariant.
3. **Deeper (optional)** — retag door-output packs off the producer at the ABI seam, so
   the producer-charged-transferred ref that L1 exploits and L2 mis-attributes stops
   being a latent mis-charge the quiesce merely masks.

**L1 is the round-2..7 sweep's final P1** — and it is the same root cause (un-quiesced
live-adapter teardown) as the RT5/J2/J3 family, now closed on the one remaining
lifecycle command.
