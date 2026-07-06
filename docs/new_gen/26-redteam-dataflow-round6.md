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
> corrections. *(This "genuinely closed" claim was itself premature — round 8's
> regression audit reopened G4 as M1; see the round-8 banner below.)*

> **Round 8 (2026-07-06) REOPENS G4 — the "genuinely closed" line above is stale.** A
> regression audit found the six fix commits clean (no regressions), and the WS
> transport / hot-reload lifetime / dispatch custody are sound — **but the round-3b
> G4 kv-race fix (`d05ed4a`) was DOC-ONLY, and its lock-skip carve-out is WRONG for
> multi-group projects**: `max_parallel` is per-group, `xi::kv()` is process-global,
> and one global script runs on every lane, so a benign author who follows the
> documented "safe to skip the lock" advice still hits concurrent `std::map` RMW / UB.
> **M1 (P2, reopens G4).** Plus M2 (P3 compile-prune deletes the live script's debug
> artifacts), M3/M4 (P3 WS doc/load), one latent note. Full writeup in the **Round 8**
> section at the end. Fix M1 before claiming closure.

> **ROUND 8 FIXED @ `bfb1993` (2026-07-06, full gate green).** M1 (reopened G4) →
> the `xi_kv.hpp` contract now states the ONLY lock-skip is a single-group,
> `max_parallel==1`, no-async project; a multi-group project (Σ lane workers > 1)
> must always lock `kv_mutex()`. M2 → the compile prune now scans for the actual
> highest same-stem version < ver (not `ver-1`), so a plugin build bumping the shared
> `s_version` no longer causes the live outgoing script's debug artifacts to be
> deleted. M3 → `close_client` notify comment corrected (writer re-checks the queue
> predicate, not `client_`). M4 → the 2nd-client reject drain is now a non-blocking
> single pass (no 400ms poll-thread stall). Latent → `write_override`'s
> no-`mu_`/poll-thread-serial dependency documented. **Not re-stamping "closed" —
> rounds 2–8 findings are all currently FIXED/DEFUSED/REFUTED, but a fresh angle has
> reopened a "closed" item twice now (G4 → still G4); treat the sweep as ongoing.**

> **Round 9 (2026-07-06) — four net-new surfaces (autoload · stream · msgpack · resource-handle).**
> No UAF. **N1 (P2, CONFIRMED)**: the machine-autoload evict/reinstate path updates
> `InstanceRegistry` synchronously but the `CapRegistry` sweep rides the pin-deferrable
> adapter dtor → a transient window where a capability with a LIVE provider returns
> `ESHAPE` on every call (dropped imgcodec decode/encode → frames fail to inspect);
> self-heals when the in-flight cap-call pin drops. The RT5/L1 lifecycle-teardown family
> again — but as a *lost-result*, not a UAF (the pin that prevents the UAF widens the
> window). **N2 (P3 doc)**: the resource-handle "five rules" omit the resolve-time pin
> that is the sole reason `lut_owner` is safe → seeds a cross-lane UAF into future
> type-owner plugins (esp. the GPU `cudaFree` variant the doc says to copy "verbatim").
> **N3 (P3 robustness, over the benign edge)**: `xi_mp.hpp` `canonicalize` `seen.reserve(e.len)`
> unbounded from an unchecked map count → wild-alloc abort at the "untrusted bytes" door.
> **N4 (P3 example)**: `qa_pack_stream`'s unbound consumer binds its stream id from a fault
> → a foreign fault kills the wrong stream. Full writeup in the **Round 9** section.

> **Round 10 (2026-07-06) — a second P1.** Four un-swept surfaces. **O2 (P1, CONFIRMED)**:
> `close_project`/`open_project` are the only two lifecycle handlers that release the
> detached-launch pause (`g.dismiss()`→`unpause`) *before* their FreeLibrary
> (`service_cmd_lifecycle.cpp:896/786` vs the teardown at `:904/:809`); every sibling holds
> the guard across the op. A source-emit thread that snapshotted the bus sink before the
> quiesce's `clear_sink` then launches a detached one-shot after the pause is dropped —
> `inflight.launch` sees `paused_==0` → the inspect runs concurrently with the FreeLibrary →
> **call into an unmapped plugin DLL** (use-after-unload; worse than L1 — the `shared_ptr`
> pin can't defend an unmapped `process_fn_`/`destroy_fn_`). Same RT5/L1 family — the pause
> is established but released one step too early. Fix: move `dismiss()` to after the teardown.
> **O1 (P3)**: the H7 await-dump fix missed the shared `recover_seh_stack_or_die` `_Exit`
> (`xi_seh.hpp:114`, ~18 call sites) → a concurrent minidump is truncated (+ 2 doc defects
> asserting it holds). **O3 (P3)**: no prepare/commit export-pairing check → a benign-but-buggy
> plugin exporting only one gets torn/lost live config. Cap shadow-stack REFUTED clean. Full
> writeup in the **Round 10** section.

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

---
---

# Round 8 — regression audit of the round-6/7 fixes + fresh-surface sweep

**Goal:** the round-6/7 fixes had just landed (`02ad9e4..c3e7f1f`) plus the round-3b
closures (`d05ed4a`, G2–G5). Fresh fixes are prime bug territory, so one probe hunted
**regressions in those six commits**; three others swept surfaces rounds 2–7 hadn't
deeply audited — the WS server + RT8 async egress, script compile/hot-reload module
lifetime, and script-KV state + atomic-IO + config. Threat model unchanged.

**Headline:** the fixes are clean (no regressions), and the WS transport / hot-reload
lifetime / dispatch custody are all sound — **but the round-3b G4 kv-race fix was
doc-only, and its doc is wrong**, so **G4 is effectively REOPENED as M1**. Everything
else is P3 (one debug-artifact-loss, two WS doc/load notes) plus a latent note.

## M1 — the G4 kv-race "fix" is doc-only and its lock-skip carve-out is **wrong for multi-group projects** → the exact concurrent-`std::map`-RMW UB it claims to prevent — **P2 (doc defect, data-race/UB consequence); REOPENS G4**
`backend/include/xi/xi_kv.hpp:36-43` (the thread-safety contract) vs `:306-309`
(`kv()` is a process-global `static Kv`), `service_dispatch.cpp:19-24, 330-332, 352`
(per-group lanes), `service_inspect.cpp:71-79, 191-193` (script copied under
`script_mu`, then the body runs **unlocked**).

The G4 fix (`d05ed4a`) added a comment telling script authors when they must lock
`kv_mutex()`. It **cannot** be code-enforced (the SDK can't force a script body to
lock), so the comment IS the contract — and the contract is wrong. It says:

> *Only a script that is BOTH single-parallelism (`max_parallel==1`) AND uses no
> async may skip the lock.*

But **`max_parallel` is per-group** while **`xi::kv()` is process-global** and the
inspect script is **one global DLL** (`g_eng.script`) that **every** dispatch lane
runs. `spawn_group_pool_` builds one lane per group (`service_dispatch.cpp:330-332`),
each with its own `max_parallel` worker set, and the oversubscribe check literally
sums `Σ lp->cfg.max_parallel` across lanes (`:352`). So a project with **two groups,
each `max_parallel==1`, no async** — which the comment calls "safe to skip the lock" —
still runs the one global script on two lane workers **concurrently**, both doing
unlocked read-modify-write on the shared `std::map<std::string,Slot>`:

```
Thread A (group camA, max_parallel==1)      Thread B (group camB, max_parallel==1)
  n = kv().get_i64("count",0)+1               n = kv().get_i64("count",0)+1   // concurrent map read
  kv().set_i64("count", n)                    kv().set_i64("count", n)        // concurrent operator[]/rebalance
```

A benign author who followed the documented carve-out omits the lock, and gets a data
race / UB on the `std::map`: **torn `count` (lost update / cross-frame KV bleed), or a
corrupted red-black tree → later crash.** This is exactly the G4 defect the comment
advertises as prevented. **Fix:** the safe condition is not "this group's
`max_parallel==1`" but "total concurrency across *all* lanes that can run the global
script is 1" — i.e. a single-group project with `max_parallel==1` and no async.
A multi-group project (`Σ` lane workers > 1) must **always** lock `kv_mutex()`. Correct
the contract (and consider a code assertion at the boundary, since it is already
global).

## M2 — compile prune "keep N-1" is defeated by the process-global `s_version` shared across compile modes → deletes the still-live outgoing script's debug artifacts — **P3 (debug-artifact data-loss + doc-correctness)**
`backend/include/xi/xi_script_compiler.hpp:701,704` (`static std::atomic<int> s_version`
inside `compile()`), `:771` (same `compile()` serves `CompileMode::PluginDev`), prune
`:957-958` keeping `ver` and `ver-1`, comment `:924-930`.

`s_version` is a single process-global counter bumped by **every** compile — inspect
scripts *and* project plugins (`xi_pm_load.hpp` `PluginDev`) *and* AOT. The prune keeps
`file_ver == ver` and `== ver-1`, and its comment asserts `ver-1` "is still the LIVE
mapped script." That holds only if versions are **consecutive per stem**. They are not:
whenever a plugin build (or a second script) consumes the intervening number, the real
previous *script* build is at `ver-2` or lower and does **not** match `ver-1`, so it is
deleted. The prune filters by same-stem prefix, so plugin artifacts in other dirs are
untouched — but the outgoing `inspect_v<k+1>` (still mapped / in-flight via the
`module_lifetime` deferral) has its `.dll` delete fail safely (sharing violation,
ignored) while its **`.pdb/.lib/.obj/.exp` are unlocked and removed**. A crash on an
inspect still running that outgoing module during the swap/in-flight window then yields
a minidump with `inspect_v<k+1>.dll+0xNNN` and **no line info** — precisely what the
comment claims to prevent. Routinely triggered by any plugin-bearing project or the
parallel-QA harness. **No memory-safety impact** (the live `.dll` cannot be deleted).
Same lineage as the J7 "keep N-1 PDB" fix, whose consecutive-version assumption the
shared counter breaks. **Fix:** track the previous version *per stem* (or scan
`script_build` for the highest existing `inspect_v<n> < ver`), not arithmetic `ver-1`
on a global counter; fix the comment's assumption.

## M3 — `close_client` notify comment misdescribes the writer's wake predicate — **P3 (doc)**
`backend/include/xi/xi_ws_server.hpp:656-658` vs the writer wait predicate `:693`.

The comment says the `out_cv_.notify_all()` at `:668` wakes the writer so "it will
re-check the now-nulled `client_`." The writer is parked on
`out_cv_.wait(lk, []{ return writer_stop_ || !out_q_.empty(); })` — it re-evaluates
only that predicate, never `client_`. After `close_client` clears the queue the
predicate is false, so the woken writer immediately re-parks; the notify is a harmless
spurious wakeup, not the "re-check `client_`" the comment claims. Behavior is correct
(the epoch guard is the real barrier), but this is load-bearing documentation for
exactly the close/writer race a maintainer will reason about — it invites a false
belief that the writer polls `client_`. **Fix:** correct the comment (or drop the
now-unnecessary notify).

## M4 — 2nd-client reject drain runs a 400 ms recv-loop on the poll thread → a benign reconnect storm delays liveness — **P3 (load, bounded)**
`backend/include/xi/xi_ws_server.hpp:493-499` (Windows path; `TODO(linux)` at `:500`).

Rejecting a second connector does `::shutdown(s,1)` then
`while (::recv(s,drain,1024,0) > 0) {}` with `SO_RCVTIMEO=400ms`, **on the poll
thread**. A benign client closes promptly (~2 recvs), but under a benign FE reconnect
storm while a client is attached, each rejected SYN can cost the poll thread up to
~400 ms in this loop — during which it services neither the attached client's inbound
commands, nor the top-of-`poll()` `drop_requested_` proactive-close check, nor the
heartbeat. Bounded per iteration (no hang), but it serializes reject latency onto the
one thread that also owns liveness. The only place a benign peer can measurably delay
the poll thread. **Fix (optional):** move the reject-drain off the poll thread, or cap
it to a single non-blocking recv.

### Latent note (not a finding — record for when command handling stops being serial)
`toolchain::write_override` (`xi_toolchain.hpp:~200`) writes `project.json` **without**
the PluginManager `mu_`, unlike `save_project_locked`. Harmless **today** because all
WS command handlers run serially on the single poll thread — but it would tear
`project.json` the moment command handling becomes multi-threaded. Flagging because it
is an invariant the codebase now depends on implicitly.

## REFUTED / verified-clean this round
- **All six round-6/7 fix commits** (`02ad9e4` rename quiesce, `5612e52` sweep-packs +
  seq_cst walkers, `9ef6b56` K3/K4/K5/K6, `e434f13` K7–K10, `c9d0b48` L2 untagged
  push, `d05ed4a` G2–G5) — **no introduced defect.** Specifically: the rename quiesce
  takes `_rn_guard` **before** `mu_`, byte-identical ordering to its siblings, no
  lock-held-across-drain and no inversion (no deadlock); the L2 untagged-retain is
  balanced by an untagged release in the emission half (`current_owner()==0`), ledger
  `rc` balances, sweep stays fail-closed both ways; the seq_cst promotion covers **all
  three** WalkGuard loops (`release_all_for`/`stats`/`stats_by_owner`); G3's
  `parallel_for` TicketScope faithfully mirrors `xi::async`'s save/restore RAII and
  closes the ticket window without shifting it. *(Minor wording nit: `5612e52`'s
  comment says the scope dtor "matches the adapter dtor's three-plane sweep" but the
  plane order differs — independent registries, identical behavior, wording only.)*
- **WS server + RT8 async egress — 6/6 named vectors REFUTED.** Stale-epoch delivery
  (null-before-clear + monotonic `conn_epoch_` under `tx_mu_`), queue-freed-under-writer
  (move-out under `out_mu_`), torn frame from concurrent producers (whole-frame enqueue
  under `out_mu_`, single writer), partial-write boundary desync (positive-return
  advance, `s<=0` → whole-client drop), stop/join vs slow client (`::shutdown` before
  `join`), and `SO_EXCLUSIVEADDRUSE` accept race all hold. No `tx_mu_`/`out_mu_` nesting
  → no inversion. `drop_requested_` cleared in `close_client` → no cross-connection
  fire.
- **Script hot-reload `module_lifetime` — VERIFIED SAFE.** The `LoadedScript` (incl. the
  `module_lifetime` shared_ptr) is copied by value under `script_mu` before any module
  deref and lives in the `RunOutcome` that outlives both compute and emission halves —
  covering the post-emit `set_run_context("")` tail. Swap defers teardown to the last
  ref; two reloads serialize under `run_mu→script_mu`; `script_generation` is
  mutex-ordered. No UAF into an unloaded DLL, no torn binding.
- **kv get-by-reference / atomic_write / config-validate** — REFUTED (`std::map` pointer
  stability holds the documented per-key contract; every `atomic_write` caller is
  poll-thread-serial or uses a dedicated path; `validate_config_against_manifest` is
  pure over its args).

## Round-8 disposition
1. **M1 (P2, reopens G4)** — correct the `xi_kv.hpp` contract: multi-group (Σ lanes > 1)
   must always lock `kv_mutex()`; the `max_parallel==1` carve-out is only safe
   single-group. Highest priority — a benign author following the current doc hits
   `std::map` UB.
2. **M2 (P3)** — per-stem previous-version tracking in the compile prune; fix the
   comment.
3. **M3 / M4 (P3)** — WS comment correction; optional reject-drain off the poll thread.
4. **Latent** — decide whether `toolchain::write_override` should take `mu_` now, or
   document the poll-thread-serial dependency explicitly.

**No new memory-unsafety this round** (the fixes hold, the transport is hardened). The
one that matters is **M1: a "closed" finding (G4) is not actually closed** — the fix was
documentation, and the documentation is wrong for the multi-group configuration the
product fully supports.

---
---

> **ROUNDS 9 & 10 ALL FIXED @ `9712aab`..`495f220` (2026-07-06, full 8-stage gate
> green).** O2 (the P1) → app-team `9712aab` moves `g.dismiss()` to AFTER
> `close_project`/`open_project` so the detached-launch pause holds through the
> FreeLibrary (use-after-unload closed; RT5/J2/J3/L1 family's 2nd P1). N1 (P2) →
> `evict_machine_provider_locked_` now `sweep_caps_for(owner)` synchronously at
> eviction (verified: doesn't break an in-flight pinned cap call, later dtor sweep is
> an idempotent no-op) — the ESHAPE burst window collapses to zero. O1 (P3, a hole in
> the round-4 H7 fix) → a layering-safe `seh_predump_drain_hook` lets
> `recover_seh_stack_or_die` drain an in-flight sibling minidump before `_Exit` (+ the
> two overclaiming comments corrected). O3 (P3) → an unpaired prepare/commit export
> now warns once + degrades to the coherent gated path (no torn swap). N2 (doc 14 —
> added rule 6: resolve must pin). N3 (`xi_mp` reserve clamped to bytes-remaining).
> N4 (`qa_pack_stream` — a fault no longer binds an unbound consumer's stream id).
> *(Per the standing lesson: NOT stamping the whole sweep "closed" — treated as
> ongoing.)*

# Round 9 — net-new surfaces (autoload · stream · msgpack codec · resource-handle)

**Goal:** four surfaces rounds 2–8 never swept — three of them (machine-autoload,
resource-handle lease, the stream chunking convention) landed *after* most rounds.
Threat model unchanged (benign, stress/race, doc defects count). Each probe had to
produce a concrete free/use interleaving or REFUTE its slice.

**Headline:** no UAF this round — but **N1 (P2) is a genuine lost-result-under-load in
the machine-autoload path** (the RT5/L1 family surfaced again, this time NOT as a UAF
but as a two-registry ordering inversion), and N2/N4 are pattern/spec defects that
*seed* concurrency bugs into future code. N3 is a robustness gap at the ingress door
that sits just over the edge of the benign mandate (flagged honestly).

## N1 — machine-autoload evict/reinstate: InstanceRegistry updated synchronously but CapRegistry sweep rides the (pin-deferrable) adapter dtor → transient `ESHAPE` burst on a capability that has a LIVE provider — **P2 (lost result under load); CONFIRMED**
`backend/include/xi/xi_pm_load.hpp:62-72` (`evict_machine_provider_locked_`),
`backend/include/xi/xi_cabi_adapter.hpp:309` (`sweep_caps_for` in `~CAbiInstanceAdapter`),
`backend/include/xi/xi_cap_abi.hpp:281-285` (`f_cap_call` lookup→resolve→ESHAPE),
`:114-119` (ETAKEN→shadow), `:180-189` (promote-on-sweep).

The **4 UAF hypotheses were REFUTED** — `f_cap_call`'s `resolve_provider_` pins the whole
adapter via a `shared_ptr` copy taken under `InstanceRegistry`'s mutex, so an evict can't
free `inst_` under a running handler; the reinit-gate + monotonic-owner-id + owner-match
close the reinstate/promote/reload races into a clean `ESHAPE`, never a wrong/dead call.

But that same pin exposes a **registry inversion**. `evict_machine_provider_locked_`
removes the provider from `InstanceRegistry` **synchronously** (`:70`, deliberately, "so
the funnel's `resolve_provider_` can't hand it out mid-teardown") while the `CapRegistry`
sweep rides the adapter dtor (`:71` shared_ptr drop → `~CAbiInstanceAdapter` →
`sweep_caps_for`). A concurrent in-flight cap call holds a pin → the dtor (and thus the
cap sweep) is **deferred arbitrarily long**. During that window:

1. Worker T-B is mid-`f_cap_call("X")` on machine provider M (owner 5), holding `pin_M`
   inside a HEAVY handler.
2. Poll thread runs `create_instance` of the same autoloaded plugin →
   `evict_machine_provider_locked_` removes M from `InstanceRegistry`. `~M` is
   pin-deferred, so `CapRegistry["X"]` **still names owner 5**.
3. The project factory's ctor calls `f_cap_register("X", …)`. "X" is still held by owner
   5 → `ETAKEN` → filed as a **shadow**, not active.
4. **Window:** `CapRegistry["X"]` → owner 5, but owner 5 is gone from `InstanceRegistry`
   and the live project provider is only a shadow. **Every new `f_cap_call("X")` does
   `lookup`→ok, `resolve_provider_(5)`→null → `XI_CAP_ESHAPE`** despite a healthy
   provider existing.
5. T-B finishes → drops `pin_M` → `~M` sweeps owner 5 → `promote_or_erase_` promotes the
   project shadow → "X" active again. **Self-heals.**

**Observable:** a transient burst of `ESHAPE` on a capability with a live provider,
bounded by the longest pre-evict in-flight cap call — and this plane is *explicitly sized
for HEAVY calls*, so the window is not negligible. For imgcodec that is dropped
decode/encode → frames that fail to run their inspection (lost results). Reachable by a
normal `create_instance` / `remove_instance` / `close_project` / `reload_machine_provider`
racing dispatch — all four share the inversion. Not a UAF, not permanent. Single-threaded
`test_cap_autoload` misses it (no pin is ever held across an evict, so the window has zero
width). **Fix direction:** make the cap-registry handoff synchronous with the
InstanceRegistry removal — `evict` should `unregister_all_for(owner)` (or hand the name
directly to the incoming provider) rather than rely on the deferred dtor sweep; that
collapses the window to zero and removes the ETAKEN-shadow detour.

## N2 — resource-handle convention: the "five rules" omit the ONE rule that makes the demo safe (the resolve-time pin), and tells the next owner to copy the lease "verbatim" → seeds a cross-lane UAF into future type-owner plugins — **P3 (doc/spec defect); CONFIRMED**
`docs/new_gen/14-lib-plugin-capability-plane.md:349-373` (the five rules) vs
`plugins/lut_owner/lut_owner.cpp:367-385` (`resolve_` pins a `shared_ptr` copy under
`mu_`, atomically with the gen check) and `:386-408` (the `gpu.buf` extrapolation).

`lut_owner` itself is **sound** — I refuted every UAF/torn/ABA candidate: `resolve_`
returns `s.obj` (a `shared_ptr<const Lut>` **copy**) under `mu_`, so a concurrent
`recycle_locked_` (`s.obj.reset()` under the same `mu_`) drops the ring's ref while the
in-flight reader keeps the object alive via its copy; generations come from a
DLL-lifetime monotonic `atomic<int64_t>`, minted once, never reused → no stale-aliases-live.

The defect is **pattern-level**. The convention credits concurrency-soundness to **rule 2
(immutable — "what makes concurrent consumers … trivially sound")** and **rule 3 (the
ring/generation lease — "a stale handle can never alias a fresh object")**. Neither
prevents a free-during-use UAF: immutability protects object *content*, not its *storage*;
the gen check only fails a **future** resolve, doing nothing for a reader that already
passed the check and is mid-deref when another lane recycles the slot. The actual
load-bearing mechanism — **the `shared_ptr` pin taken under the lock at resolve** — is in
none of the five rules; it lives only in a private impl comment (`lut_owner.cpp:68-72`). A
future owner implementing rules 1-5 literally but resolving to a raw pointer and freeing on
recycle (fully rule-compliant) has a textbook cross-lane UAF/double-free. The doc's own
`gpu.buf` VRAM sketch says the pattern carries over "verbatim … ring/generation lease over
the arena" where recycle = `cudaFree` — with no `shared_ptr` analogue, that is a
device-memory use-after-free under ring pressure + concurrent consumers. **Fix:** add a
sixth rule — *"Resolve must pin the object (refcount / deferred-free) for the duration of
the call, taken atomically with the gen check under the same lock; the generation lease
governs future resolves only, not an in-flight one"* — and correct the rule-2/rule-3
"trivially sound" claims. **Secondary (P3):** `lut_owner.cpp:427-479` `build_` checks dedup,
constructs outside the lock, then re-leases without re-checking dedup → two concurrent
same-content builds both miss and double-build (one orphaned duplicate), contradicting the
doc's "build counter pinned at 1" / "builds ONCE". Not memory-unsafe; doc overstatement.

## N3 — `xi_mp.hpp` `canonicalize()`: unbounded `seen.reserve(e.len)` from an unchecked map count → wild allocation → process abort — **P3 (robustness / doc-invariant mismatch; trigger is malformed input — over the benign edge, flagged)**
`backend/include/xi/xi_mp.hpp:536-537` (the `Kind::Map` case in `walk_canon`) vs the
codec's own "not a DoS" comment `:452-455`, reached from `xi_ingress.hpp:104`
(`canonicalize_entry`, the door the header at `:70` labels the **"untrusted bytes"**
boundary).

`walk_canon` does `std::unordered_set<std::string_view> seen; seen.reserve(e.len);` where
`e.len` is the declared map count (up to `0xFFFFFFFF` for `map32`), **before** the loop
reads a single entry. `reserve(0xFFFFFFFF)` requests tens of GB → `std::bad_alloc` out of a
`noexcept`-free path with no caller try/catch → process abort (or OOM thrash on an
overcommit allocator). A 5-byte buffer `DF FF FF FF FF` triggers it. The codec's **own**
comment at `:452-455` asserts "a bogus huge count is NOT a DoS … bounded by the buffer
size, not the declared count" — true for the *loop* (and for the sibling `validate()`,
which has no `reserve`), but the strict `canonicalize()` path violates the very invariant
the comment claims. Every *offset/payload* read in the file is correctly bounds-checked; this
`reserve` is the sole gap.

**Scope caveat (honest):** the trigger requires a *malformed* count field, which is
malicious-input hardening — **outside** the benign stress/race mandate. Under benign
producers `e.len` is the real (small) count. Reported anyway because (a) the codebase itself
labels this door "untrusted bytes," so the hole is a legitimate hardening gap *there*, (b) a
benign disk/wire corruption of a replay file is a plausible non-adversarial path, and (c) it
defeats the codec's own documented DoS-safety guarantee and the fix is one line:
`seen.reserve(std::min<size_t>(e.len, remaining()));`.

## N4 — `qa_pack_stream` example: an UNBOUND consumer binds its stream identity from a FAULT → a foreign fault delivered first kills the wrong stream (reintroduces the F3 contamination in the unbound window) — **P3 (example / teaching-pattern defect); CONFIRMED**
`examples/qa_pack_stream/inspect.cpp:137-140` (the F3 fault branch).

The core does **not** reassemble streams — `$stream/$part/$eof` is a pure convention
(`xi_pack_contract.hpp:63-72`); reassembly lives entirely in the script, so the
shared-buffer / unbounded-growth / unlocked-map hunt items are N/A to core (verified: the
only host-side reassembly is the WS RFC-6455 fragment buffer, which is poll-thread-only +
cap-enforced + reset on close — sound). The defect is in the **example** consumer that
authors copy:

```cpp
const long long fsid = p.get_i64("$stream").value_or(-1);
if (id < 0 && fsid >= 0) id = fsid;      // "first thing seen binds the stream"
if (fsid >= 0 && fsid == id) abort(...); // treat as this stream's poison
```

On a shared lane carrying two streams A and B: an A-consumer still **unbound** (`id<0`)
that sees a legitimate fault for stream **B** first binds `id = B` (line 138), then aborts
itself with B's reason (line 140); A's real chunks then arrive with `sid != id` →
`stream_mismatch` on every one → **stream A is lost, carrying B's fault reason**, though A
never faulted. This is exactly the cross-stream contamination the F3 fix prevents *after*
binding, reintroduced through the *unbound* window — the `id<0` branch binds identity from
the fault itself, so an unbound consumer can't tell "my poison" from "someone else's." QA
misses it because every test feeds a real chunk 0 before any fault. The doc-18-canonical
design (a state machine **per `$stream` id**, keyed by the arriving chunk) does not hit
this. **Fix direction:** bind `id` only from a non-fault chunk; ignore any fault while
`id<0`. **Minor (P3 spec-tidiness):** `propagate_fault` carries `$eof` onto the minted
fault pack (`xi_pack_contract.hpp:156-159`), so a non-last pack can carry `$eof=true`,
nominally violating doc-18's "`$eof` on the last chunk only" — harmless because every
compliant consumer checks `is_fault()` first.

## REFUTED / verified-clean this round
- **Machine-autoload — 4 UAF hypotheses REFUTED.** The `shared_ptr` pin +
  `cap_reinit_gate` shared-lock re-lookup + `owner==adapter->owner_id()` match +
  monotonic-never-reused owner ids close evict-mid-call, reinstate-vs-call,
  reload-vs-call, and shadow-promotion-vs-call into `ESHAPE`, never a UAF. (Only N1 —
  the registry-inversion lost-result — survives.)
- **`lut_owner` resource-handle impl — REFUTED.** gen-wrap/reuse, recycle-during-deref,
  torn pointer/gen, dump-vs-free all closed by the resolve-time `shared_ptr` pin + the
  never-reused generation source. (The defect is the *spec*, N2, not the demo.)
- **msgpack offset/payload reads — REFUTED clean.** `take_be` / `read_str` / `read_bin`
  / `read_ext` / `read_fixext` all check `remaining()`/`p_>=end_` before advancing; every
  `(uint32_t)len` narrowing occurs after `len <= remaining()` is proven. No OOB, no
  size-arithmetic overflow. (Only the `reserve`, N3, is the gap.)
- **Stream core — REFUTED (nothing to race).** No host/core stream reassembly buffer
  exists; `propagate_fault` correctly carries `$stream/$part/$eof`; doc-18's lane-config
  concurrency claims are accurate against `service_dispatch.cpp:356-400` + `xi_emit_gate`;
  WS fragment reassembly is poll-thread-only and cap-enforced.

## Round-9 disposition
1. **N1 (P2)** — collapse the autoload registry-inversion window: `evict` should
   `unregister_all_for(owner)` synchronously with the InstanceRegistry removal, not via the
   pin-deferrable dtor. Highest priority — lost frames under a normal create/remove-under-load.
2. **N2 (P3 doc)** — add the sixth "pin at resolve" rule to the resource-handle convention
   before any new type-owner plugin (esp. the GPU variant) is built off it; fix the
   rule-2/rule-3 over-credit.
3. **N3 (P3 robustness)** — one-line `reserve` cap; closes the codec's own documented
   DoS-safety guarantee at the untrusted-bytes door (over the benign edge, do it opportunistically).
4. **N4 (P3 example)** — fix the example's unbound-fault binding so authors don't copy the
   cross-stream-contamination pattern.

**One P2 (N1), three P3 (N2/N3/N4), no UAF.** N1 is the notable one — the un-quiesced /
non-atomic-lifecycle root cause that produced UAFs in earlier rounds shows up on the newest
lifecycle path (machine-autoload) as a *lost-result* inversion instead, because the pin that
now prevents the UAF is what widens the window.

---
---

# Round 10 — un-swept surfaces (watchdog/_Exit · prepare/commit · cap shadow-stack · project teardown)

**Goal:** four surfaces rounds 2–9 hadn't swept as a unit — the watchdog-slot/`_Exit`/crash-dump
interplay, the ABI-v7 prepare/commit staged config swap, the cap shadow-stack promote/erase
machinery (distinct from N1's eviction inversion), and the full project open/close/reload
teardown ordering. Threat model unchanged.

**Headline: O2 is a CONFIRMED P1** — `close_project`/`open_project` drop the detached-launch
pause *before* their FreeLibrary, reopening a use-after-**unload** window. Same RT5/J2/J3/L1
family (a live-DLL teardown without the launch gate held through the unload), and the sweep's
second P1. Plus O1 (P3 watchdog await-dump hole) and O3 (P3 prepare/commit export pairing).
The cap shadow-stack machinery REFUTED clean.

## O2 — `close_project` / `open_project` release the detached-launch pause (`dismiss()`→`unpause`) BEFORE the FreeLibrary → a straggler source-emit one-shot calls into an unmapped plugin DLL — **P1 (use-after-unload); CONFIRMED**
`backend/src/service_cmd_lifecycle.cpp:896` (close) and `:786` (open) — the guard is scoped to a
throwaway block with `g.dismiss()` **on the same line**, so the pause is dropped before the
destructive teardown at `:904` / `:809`. Contrast: `dismiss()` (`service_internal.hpp:363-366`)
calls `g_eng.inflight.unpause()`.

**The gate that gets dropped too early.** `quiesce_dispatch_for_lifecycle_op_`
(`service_dispatch.cpp:712-746`) is explicit (`:716-723`): a one-shot dispatch runs on a
**source plugin's own emit thread** (`bus sink → dispatch_one_shot_ → inflight.launch`), not the
handler thread, so "without this a source emitting mid-op could launch an inspect that calls into
a DLL being unloaded (use-after-unload). `clear_sink` stops future fires; `pause()+drain()` is the
Dekker handshake that also catches an emit **already past the sink read but not yet counted**."
The design REQUIRES `paused_` to stay set through the FreeLibrary — `drain()` alone can't catch
the not-yet-counted straggler; only the `paused_` bail at `xi_inflight_runs.hpp:73` can.

**Why close/open are the only two that break it.** Every sibling DLL-unloading op holds the
guard across the destructive call (`remove_instance` `service_cmd_project.cpp:304`,
`create_instance` `:274`, `rename_instance` `:328`, `set_instance_def`, `commit_group`,
`commit/discard_working_copy`, `rescan/export/recompile/rebuild`) — their `resume()`/`dismiss()`
(and its `unpause`) fire *after* the op. Only `close_project`/`open_project` `dismiss()` *before*.
The intent was benign — `dismiss()` skips the continuous-resume (there is no project to stream
after a close) — but `dismiss()` *also* unpauses launches, and that side effect drops the gate
early. `close_project` itself (`xi_pm_project.hpp:66-109`) never touches `g_eng.inflight`, so
after `dismiss()` there is no protection at all.

**The interleaving (close; open identical):** continuous mode, a source actively emitting.
1. Source emit thread T (inside the plugin DLL) enters `emit_pack`, snapshots `to_fire = sink_`
   under `mu_` (`xi_trigger_bus.hpp:157`) — a stack-local `std::function` copy, valid regardless of
   any later `clear_sink()` — then is descheduled before invoking it.
2. `cmd_close_project_:896` runs the quiesce: `pause()` (paused_=1) → `clear_sink()` →
   `stop_dispatch_pool_()` (joins workers, real wall time) → `drain()` returns immediately
   (`count_==0`, T hasn't launched yet) → returns.
3. `g.dismiss()` → **`unpause()` → paused_=0**; the block ends.
4. `:902-903` `clear_sink()`/`reset()` again — a no-op for T (it holds its private `to_fire`).
5. `:904 close_project()` → `instances.clear()` destroys adapters + owner sweeps, then
   `FreeLibrary`s the plugin DLLs (`xi_pm_project.hpp:95`).
6. T resumes, invokes `to_fire(ev)` → `dispatch_one_shot_` → `inflight.launch(...)`. The gate at
   `xi_inflight_runs.hpp:73` sees `shutting_(false) || paused_(0)` → **both false → detached
   inspect spawned.**
7. That inspect runs concurrently with step 5's FreeLibrary. Even if it `find()`-pins the adapter
   `shared_ptr` before `instances.clear()`, the pin keeps the adapter *object* alive but its
   `process_fn_`/`destroy_fn_` point into a DLL `FreeLibrary` has now **unmapped** → call into
   unmapped memory (AV) on `run_pack_door`, and again on the pinned adapter's eventual
   `~CAbiInstanceAdapter → destroy_fn_`.

**Observable:** access violation calling an unmapped project-plugin DLL (or UAF into a
freshly-swept pack). This is exactly the use-after-unload the `close_project` comments
(`xi_pm_project.hpp:72-81`) reason about for the *synchronous* order but miss for the
*concurrent* detached one-shot the dropped pause lets through. **Worse than L1** — L1 was
use-after-free of a pack; this is use-after-**unload** of the whole plugin DLL, which the
`shared_ptr` pin cannot defend. Process-exit is safe (`controlled_shutdown_teardown_` uses the
*terminal* `begin_shutdown()`, held through FreeLibrary — `service_dispatch.cpp:570,616`); only
the interactive close/open and the `reopen_fresh_working_copy` reload cycle are exposed.
Load-sensitive: the window is a single deschedule of T between the sink read and the launch CAS —
benign under no load, realized under stress (more emit threads than cores). Reproducible via
`harness_open_close_cycle.py` with a live source emitting at high fps during close/open.
**Fix:** hold the guard across the teardown like every sibling — move `g.dismiss()` to *after*
`plugin_mgr.close_project()` (and after `open_project(...)`), so `paused_` stays set through the
FreeLibrary and a straggler launch bails at `xi_inflight_runs.hpp:73`. (`dismiss()` after the op
still skips the unwanted continuous-resume.)

## O1 — the H7 fix is incomplete: the shared `recover_seh_stack_or_die` hard-exit omits `await_dump()` → truncates a concurrent minidump — **P3 (forensic loss under double-fault + 2 doc defects)**
`backend/include/xi/xi_seh.hpp:106-115` — `std::_Exit(kStackGuardExitCode)` at `:114` with no
`xi::crash::await_dump()` first.

Round-4's H7 added `await_dump(10000)` before every self-inflicted `_Exit` so a sibling thread
mid-`write_minidump` can finish. It is present at exactly three sites (`service_main.cpp:800`
watchdog hard trip, `service_inspect.cpp:222` inline stack-overflow, `service_dispatch.cpp:603`
drain timeout) — but **not** at the shared `recover_seh_stack_or_die` helper, which is the
unrecoverable-guard-page `_Exit` for ~18 call sites running untrusted plugin/script code
(`xi_parallel.hpp` OMP worker, `xi_async.hpp` async worker, `xi_cap_abi.hpp` cap handler,
`service_sinks.cpp` pack door / `exchange`, the `service_cmd_*` prepare/commit/exchange sites).
Interleaving: an unmanaged plugin thread takes an AV → `SetUnhandledExceptionFilter` →
`write_minidump` mid-`MiniDumpWriteDump` (slow); concurrently a dispatch worker in that plugin's
door deep-recurses → `STACK_OVERFLOW` → `_resetstkoflw()` fails → `recover_seh_stack_or_die` →
immediate `_Exit`. The dump is truncated **and** its `.json` sidecar (written after the `.dmp`)
is entirely absent → `cmd:crash_reports` finds a dump with no readable report. **Structural, not a
typo:** `await_dump` lives in `xi_crash_dump.hpp`, which `#include`s `xi_seh.hpp` (the lower
layer), so the helper can't call it without a layering-safe hook — which is why H7 patched the
three higher-level call sites and missed the shared helper. **Doc defects:** `xi_crash_dump.hpp:198-199`
claims "the watchdog / drain / **overflow** hard-exit paths call `await_dump()` before `std::_Exit`"
— false for this overflow helper; `xi_seh.hpp:99-105` claims the helper makes "the same trade the
watchdog HARD trip makes," but the watchdog trade includes `await_dump` and this one omits it.
**Fix:** register a drain callback (function-pointer / `std::atomic`) from the crash layer into the
seh layer, or lift `await_dump` into each `recover_seh_stack_or_die` call site.

## O3 — no prepare/commit export-pairing validation → a benign-but-buggy plugin exporting only one of the pair gets torn / lost live config — **P3 (missing load-time contract check)**
`backend/include/xi/xi_cabi_adapter.hpp:277-278` (independent `GetProcAddress` for `prepare_fn_`
and `commit_fn_`), `:433` (prepare's staged-vs-gated decision), `:447` (commit's) — the decision is
made **independently** in each method, and nothing at load enforces the two exports come as a pair
(the pairing lives only inside the `XI_PLUGIN_STAGED` macro). Constructible in-tree:
`backend/tests/reentry_probe.cpp:48` exports `xi_plugin_commit` with no `xi_plugin_prepare`.

- **commit-only plugin:** `prepare()` sees `prepare_fn_==null` → falls to the gated
  `InstanceBase::prepare` (`set_def`), which makes the new config immediately live; the plugin's
  staging slot is never populated. `commit_group` → `commit()` sees `commit_fn_!=null` → swaps the
  **never-prepared** (empty/stale) staging slot over live → **torn / reverted config**, and
  `commit_group` still reports `ok:true` with `get_def()` echoing garbage. (And because `prepare_fn_`
  is null, the `set_on_fault` combo-guard does NOT downgrade reinit, so `on_fault=reinit` would
  restore `committed_def_` and mask it on the next fault.)
- **prepare-only plugin:** prepare stages + caches `committed_def_`, then `commit()` no-ops
  (`commit_fn_==null`) → the staged config **never goes live**, silently dropped.

Not a race (the three race hypotheses — commit-vs-`process`, ungated `committed_def_` write vs
reinit read, back-to-back prepare on the single slot — are all **REFUTED** by the single-threaded
command plane + the reinit combo-guard). It is a missing contract check reachable by a benign-but-
buggy plugin. **Fix:** at adapter construction, if exactly one of `prepare_fn_`/`commit_fn_` is
present, refuse the load with a reason (like the ABI/caps gates) or force both null (drop to the
fully-gated `set_def` path) and warn, so the staged-vs-gated decision can never differ between
`prepare()` and `commit()`.

## REFUTED / verified-clean this round
- **Watchdog per-inspect slot — REFUTED.** Slot claim is a `compare_exchange_strong(0, deadline)`
  (no double-arm); disarm stores 0 then the owner sets `wd_slot=-1` (no free-while-arm overlap);
  the hard-trip matches on slot index AND exact deadline value (a reused slot's future deadline
  can't equal the stuck one's stale deadline — no ABA kill); `wd_deadlines` is
  `atomic<int64_t>[64]`, 8-byte aligned (no torn read); disarm-vs-fire only kills a target that
  stayed overrun through the full 1000ms cooperative grace (documented policy, not a spurious kill).
- **Cap shadow-stack promote/erase — REFUTED clean.** Every mutator + `lookup` under the single
  `mu_`; `lookup` copies the whole `Entry` by value (no torn owner/handler); `unregister`+`promote`
  are one critical section; `unregister_all_for` phase-1 (drop this owner's shadows) precedes
  phase-2 (promote), so a swept owner's shadow can't reinstate a dead owner; per-owner dedup bounds
  the shadow list (no refcount under/overflow); a promote-to-dead-owner is only ever the *transient*
  N1 ESHAPE (self-heals via the pending sweep), never permanent. Two load-bearing invariants to
  protect under future edits: (1) phase-1-before-phase-2 in `unregister_all_for`; (2) every instance
  destruction routes through `sweep_caps_for`. *(Non-finding, out of threat model: `CapRegistry::instance()`
  re-flags `g_cap_registry_alive=true` on every call and only the sweep path checks it, so the
  funnel/registration entry points call `instance()` unguarded — a process-exit static-destruction-order
  concern, not a stress/race defect.)*
- **prepare/commit compliant path — REFUTED** (single-threaded command plane + reinit combo-guard;
  only O3, the asymmetric-export case, survives).

## Round-10 disposition
1. **O2 (P1) — FIX APPLIED (local, uncommitted).** `service_cmd_lifecycle.cpp`: the
   `close_project`/`open_project` handlers now keep the quiesce guard alive across the teardown
   and call `dismiss()` *after* `plugin_mgr.close_project()` / `open_project(...)`, so `paused_`
   holds through the FreeLibrary and a straggler one-shot bails at `xi_inflight_runs.hpp:73`.
   `dismiss()` after the op still skips the unwanted continuous-resume (close = no project;
   open = new project autostarts). Only behavioral change: the launch pause is released a few
   statements later. Verified: `xinsp_backend` builds+links clean; `project_versioning` +
   `lifecycle_asserts` ctests pass. Full 8-stage gate NOT yet run; not committed — awaiting review.
2. **O1 (P3)** — a layering-safe drain hook so `recover_seh_stack_or_die` awaits an in-flight dump;
   fix the two doc defects that assert it already does.
3. **O3 (P3)** — reject or normalize an asymmetric prepare/commit export at load.

**One P1 (O2), two P3 (O1/O3).** O2 is the sweep's second P1 and the same root cause as L1 — a
live-DLL teardown that lets a straggler in — this time not because the quiesce was missing (L1) but
because it was *released one step too early*.
