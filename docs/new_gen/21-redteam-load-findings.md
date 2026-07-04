# 21 — Red-team load/concurrency findings

Status: **landed** on `polaris2/redteam-hardening` (2026-07). An adversarial
load/concurrency pass over the v12 core (branch `polaris2/the-cut-v12` @
`c9d8647`). **Threat model: well-behaved clients under heavy load** — no
malformed input or malicious peers, just the timing/contention corners that only
show up at scale. Each CONFIRMED finding below has a fix and a reproducing
regression; the two subtle concurrency fixes (RT6/RT7) are flagged for human
review with their design options laid out.

Severity legend: **High** = data-plane incorrectness or a memory-safety hazard
under load · **Med** = a correctness/observability defect a heavy run can hit ·
**Low** = latent / narrow.

---

## Confirmed findings (fixed + regression each)

### RT1 / F1 — `propagate_fault` drops stream identity  · Med · CLEAR

**Where.** `backend/include/xi/xi_pack_contract.hpp` `propagate_fault()`.

**Repro.** Seal a fault pack carrying the doc-18 stream keys
(`$stream`/`$part`/`$eof`) and run it through a funnel hop (`use().process()` →
the short-circuit that mints the propagated fault). The propagated fault carried
`$fault*/$seq/$src/$prov` but **not** `$stream/$part/$eof`.

**Impact.** doc 15's poison marker is "a `$fault` pack carrying the stream's
`$stream` id poisons the whole stream." After one hop the propagated fault lost
that id, so a mid-stream fault silently stopped poisoning its stream — the
consumer would then time out (or worse, accept stragglers) instead of aborting.

**Fix.** Copy `kStream`/`kPart` (i64) and `kEof` (bool) forward when present,
exactly as `$seq` is carried. Cheap, presence-gated, no payload copied.

**Test.** `plugins/use_pack_door_test.cpp` §8b: a fault sealed with
`$stream=1002/$part=2/$eof` short-circuits a hop; assert all three survive on the
propagated fault (and the door never ran).

---

### RT2 / F3 — stream consumer poisoned by a FOREIGN fault  · Med · CLEAR

**Where.** `examples/qa_pack_stream/inspect.cpp` consumer state machine.

**Repro.** Deliver a `$fault` tagged for stream A to a consumer reassembling
stream B (a shared lane carries faults for every stream on it). The consumer
called `is_fault()` → `abort()` **before** checking the fault's `$stream`, so any
fault — even one for another stream, or a stream-less one — cleared B's
in-progress reassembly.

**Impact.** On a shared ordered lane, one stream's poison wrongly aborts every
other live stream's consumer.

**Fix.** doc 15's marker requires the fault to NAME the stream it poisons. The
consumer now poisons only when the fault's `$stream` matches its own id; a fault
for another stream, or a stream-less fault, is ignored (reassembly untouched). A
first-seen fault still binds the consumer's id (so a stream that opens with its
own poison still aborts).

**Test.** `examples/qa_pack_stream` stream 1004: mid-flight it receives a fault
for stream 1002 and a stream-less fault, then completes normally with both
features found exactly once. Surfaced as `s4_foreign_fault_ignored` and asserted
by `driver.py`.

---

### RT3 / F5 — `record_replay` injects `$channel/$seq` unconditionally  · Med · CLEAR

**Where.** `plugins/record_replay/record_replay.cpp` + the XEX1-v3
encoder/parser (`plugins/expose/src/xex1_encode.hpp`, `xex1_pack_dump.hpp`,
`xex1_pack_parse.hpp`).

**Repro.** Record a pack that carries **no** `$channel`/`$seq`, replay it, compare
entry counts. record_save lifted those reserved keys to the frame header
(defaulting to `"default"`/`0` when absent) and record_replay re-injected them
unconditionally — so the replayed pack gained two entries the original never had
(`count()` drift; a round-trip that claims entry-for-entry fidelity was lossy).

**Impact.** Replayed packs are not faithful to the recorded pack; anything keying
on the presence of `$channel`/`$seq` (routing, ordering) sees phantom keys.

**Fix.** Make the v3 header fields OPTIONAL. `encode_frame_v3`/`encode_pack_v3`
take `has_channel`/`has_seq` (default **true**, so expose's wire + every golden
is byte-identical); record_save passes the pack's ACTUAL presence
(`in.has(kChannel/kSeq)`), so a channel/seq-less pack is persisted without them.
The parser signals `ParsedFrame::has_channel`/`has_seq`; record_replay injects
each only when the file carried it. A frame with both present is still the
`map(4)` `{v,channel,seq,frame}` shape — unchanged bytes.

**Test.** `plugins/record_replay_pack_test.cpp` §G: save a 3-entry pack with no
`$channel/$seq`, replay, assert neither key was injected and
`replayed.size() == original.size()`.

---

### RT4 / B1 — 2nd provider instance → capability permanently lost  · High · CLEAR

**Where.** `backend/include/xi/xi_cap_abi.hpp` `CapRegistry`.

**Repro.** Create two project instances of the same capability-provider plugin.
The 2nd's factory re-registers the same names and got `XI_CAP_REG_ETAKEN`
**silently** (the plugin doesn't act on it) → live-but-unregistered. Remove the
1st instance: its owner-sweep drops the name, leaving a live provider with NO
registration and the capability resolving to nothing — permanently.

**Impact.** A capability disappears while a perfectly good provider for it is
still running; only a reload recovers it.

**Design choice (why ref-count, not reject).** The finding offered (a) reject the
duplicate loudly, or (b) ref-count the name so instance 2 re-registers when
instance 1 leaves. **Chose (b).** Rationale: (a) can't be done cleanly host-side
(registration happens inside the plugin factory; the host never sees the plugin's
register return, and not every plugin is a declared provider), and it can't
satisfy the finding's own regression ("two instances coexist, remove the first,
the capability still resolves"). (b) fixes the loss completely AND preserves the
existing `ETAKEN` contract that `test_cap_plane` pins.

**Fix.** `CapRegistry` keeps, per name, a primary entry plus a **shadow stack** of
losing (owner, handler, self) registrations. A same-owner re-register overwrites
(the reinit path). A different-owner register is still refused with `ETAKEN` to
the caller **but remembered** as a shadow. When the active holder unregisters or
is swept, the most-recent shadow is **promoted** into the name. Shadows for a
dying owner are swept too, so promotion only ever installs a live provider.

**Test.** `backend/tests/test_cap_provider_refcount.cpp`: instA holds `test.echo`;
instB is `ETAKEN`-shadowed; remove instA → the cap still resolves and instB now
serves it; remove instB → the name is gone, registry balances.
`test_cap_plane`'s `ETAKEN` + loser-sweep section is unchanged (still green).

---

### RT5 — `cmd_remove_instance_` has no quiesce guard  · High · CLEAR (shared root cause)

**Where.** `backend/src/service_cmd_project.cpp` `cmd_remove_instance_`.

**Repro / analysis.** Every DLL-affecting lifecycle op
(recompile/rebuild/commit/discard/export/open/close, and rename-via-reload)
wraps itself in `quiesce_dispatch_for_lifecycle_op_` (service_dispatch.cpp) —
pause detached launches, drop the bus sink, stop + drain the dispatch pool — so
no worker is inside plugin code when the DLL/instance is torn down.
`cmd_remove_instance_` was the **only** one that didn't. Removing an instance
destroys its runtime object (adapter dtor → `xi_plugin_destroy` + pack/cap owner
sweeps) while dispatch workers and backpressured in-flight events may still hold
owner-tagged refs to it.

**Impact.** Closes the PLAUSIBLE pack-registry ledger mis-attribution UAF (a
guard-less worker releasing an owner-tagged ref leaves a phantom ledger bucket
the sweep later cashes while a backpressured event still holds the pack), and
narrows the cap-plane transient windows. This is the reachability fix for backlog
R1.

**Fix.** Wrap the body in
`auto _rm_guard = quiesce_dispatch_for_lifecycle_op_("remove_instance", &srv);`
— resume (default) on scope exit so continuous streaming for the remaining
instances comes back.

**Test.** `examples/qa_remove_under_load/` — mock_camera (PACK MODE) self-drives a
continuous stream; the inspect script chains every trigger pack into a churned
`victim` instance's door (keeping its refs in the dispatch flow) while the driver
hammers `remove_instance`/`create_instance` on `victim` under load. Asserts
survival (no dropped socket / crash), pool resumption (frames keep flowing after
each quiesce), and a consistent instance set. A deterministic UAF repro needs
ASan; this is the load smoke that would catch a crash/hang from the race.

---

## Design-decision findings (implemented behind a test — **human review**)

### RT6 / A1·A2 — capability funnel reinit-UAF  · High · CONCURRENCY

**Where.** `backend/include/xi/xi_cap_abi.hpp` `f_cap_call` (the funnel) +
`backend/include/xi/xi_cabi_adapter.hpp` `reinit()`.

**The race.** The funnel runs the provider handler under **no CallScope**
(providers contract to be thread-safe, so heavy cap calls run concurrently).
`reinit()` (on_fault=reinit) rebuilds the instance and calls `destroy_fn_(old)`;
its own `CallScope` gates only the 6 data-plane entries (and is a no-op for a
reentrant-unlimited provider anyway). The `shared_ptr` the funnel pins
(`resolve_provider_`) keeps the **adapter** alive, but not `inst_` — so a
concurrent reinit can free `inst_` (== the handler's `self`) while a cap handler
is executing against it. Use-after-free.

**Options considered.**
- **(a) Give the funnel a CallScope** so reinit excludes in-flight cap calls.
  *Rejected:* `CallScope` is admission-control keyed on `effective_cap_`, which is
  **0 (no-op)** for a reentrant-unlimited provider — exactly the provider shape the
  cap plane is built for — so it would not actually exclude anything, and forcing
  a cap on providers would kill the concurrency the plane is sized for.
- **(b) Refcount `inst_` and defer `destroy_fn_(old)` until in-flight handlers
  drain.** Correct, but a hand-rolled inflight counter has a read-vs-enter race
  (the funnel resolves `self` from the registry before incrementing) that needs
  careful ordering to be sound.
- **(c) Pin `inst_` (not just the adapter) for the handler duration.** Same intent
  as (b); the question is the primitive.

**Chosen: a reader/writer "reinit gate" (a refined (b)/(c)).** A per-adapter
`std::shared_mutex cap_gate_`. The funnel takes the **shared** side around the
handler run **and re-resolves the live handler/self under it**; `reinit()` takes
the **exclusive** side around `destroy_fn_(old)`. Because reinit re-registers the
fresh `self` (in the factory) *before* it swaps + destroys, a funnel under the
shared lock either sees the fresh registration (calls fresh) or an old-but-pinned
one (the exclusive destroy waits for it to drain) or a mid-sweep gap
(`EUNKNOWN`). No handler ever runs against a freed `inst_`.

**Why this one.** Reader/writer keeps concurrent HEAVY cap calls fully parallel
(the doc-14 sizing doctrine) — the exclusive lock is taken only on the rare
fault/reinit path. It is obviously correct to review (standard RW exclusion)
versus a bespoke counter with subtle ordering. Reentrancy can't self-deadlock:
same-instance re-entry is refused (`-5`) before any lock, and reinit is applied
*before* the funnel enters the handler (the reinit-triggering thread doesn't hold
the shared side).

**Tradeoffs / review notes.** (1) A `shared_lock` acquire now sits on the cap hot
path — cheap, uncontended in the common case, but it IS new hot-path work. (2) The
funnel now does a second `CapRegistry::lookup` under the lock (re-resolve) — one
extra map lookup per call. (3) If a provider handler blocks for a long time, a
concurrent reinit's exclusive acquire waits that long before destroying the old
`inst_` — acceptable (reinit is a fault-recovery path, not latency-critical), but
worth noting. **Flagged for human review** — this is a core concurrency change.

**Test.** `backend/tests/test_cap_reinit_race.cpp`: 6 reader threads hammer
`test.echo` through the funnel while a writer thread `reinit()`s the same adapter
400×. Asserts coherent results (`x_echo` always right), no deadlock, service
continuity, and pool/pack balance. UAF-detection is strongest under ASan/TSan;
in Release it is a deadlock/crash net.

---

### RT7 / P1 — `cmd:stop` verdict/actuation divergence  · High · ORDERING

**Where.** `backend/src/service_inspect.cpp` `emit_run_outcome_` +
`backend/include/xi/xi_emit_gate.hpp`.

**The bug.** On the emit half, `run_result` (the verdict) was emitted guarded by
`inspect_ok` only, while the staged ordered-sink flush (PLC actuation / expose)
was guarded by `my_turn`. On a **stop-wake** (the lane stopped before this seq's
ordered turn, so every parked seq wakes at once with `my_turn==false`), a
successful frame reported its verdict to the wire while its staged sink push was
drain-dropped by the `StagedEmitGuard`. A downstream reads a PASS that implies a
PLC actuation that never happened.

**Options.** (a) On a stop-wake, also suppress the `run_result` so verdict and
actuation are consistently NOT delivered. (b) Deliver the staged push even on a
stop.

**Chosen: (a).** (b) is unsafe by construction — the staged flush is skipped on a
stop-wake precisely because a stop wakes every parked seq at once, so flushing
would deliver out of frame order and concurrently to the same sink (the exact
thing the ordered gate exists to prevent). Consistency is the goal, and the run
is being torn down anyway, so suppressing both is the safe, coherent choice: a
verdict must never imply an actuation that didn't happen.

**Fix.** One predicate — `xi::emit_success_outputs(inspect_ok, my_turn)` — now
gates BOTH the staged flush AND the verdict/`run_finished` emission, so they can
never diverge again by construction. A failed inspect still emits its
`XI_SYS_CRASHED` Result on the separate crash path (it has no staged actuation to
be inconsistent with, and the stream stays gap-free there).

**Tradeoffs / review notes.** A successful frame that is stop-woken now emits
**nothing** for its seq (no verdict, no `run_finished`) — the stream has a gap at
teardown for any frame caught mid-flight by the stop. That is intended (the lane
is stopping; a gap is honest), but it IS a wire-behaviour change from the pre-fix
"verdict-without-actuation." Consumers that counted a verdict per admitted frame
across a stop will see one fewer at the stop boundary. **Flagged for human
review.**

**Test.** `backend/tests/test_emit_gate.cpp` §P1: pins the predicate truth table,
and under the real stop-wake lane shape (a permanent hole parks N seqs, a stop
wakes them all `my_turn=false`) asserts every parked seq delivers verdict and
actuation **together** — here, both suppressed — so a verdict never rides out
without its actuation.

---

## Lower-priority / not fixed

### P2 — a slow consumer stalls the whole ordered lane  · Med · NOT FIXED (design note)

`send_frame` holds `tx_mu_` and blocks in `::send` **inside** the EmitTurn window,
so one slow WS consumer backs up the ordered emit for the entire lane: every
other frame's ordered turn waits behind the slow socket's kernel send buffer.
This is a real head-of-line stall under load, but the fix is a larger restructure
— decouple the wire send from the emit gate (stage the encoded frame under the
turn, then send OUTSIDE `tx_mu_` / the gate) or give each consumer its own bounded
queue with a drop/slow policy. That is a risky rewrite of the hot send path and is
**deliberately not attempted here**; it is tracked as doc 19 RT8. Recommended
direction: claim the ordered turn, snapshot+stage the bytes, `complete()` the
turn, THEN send per-consumer off the gate (a slow consumer then only stalls
itself, and back-pressure becomes a per-consumer queue policy).

---

## The rest of the record (completeness)

The pass also triaged items that did **not** become fixes, recorded here so the
audit trail is complete:

- **PLAUSIBLE, not reproduced (left as backlog / watch):**
  - **A3** — a fault charged to a lib instance during a concurrent config swap
    could apply on_fault against a half-committed def; narrow and not reproduced
    under the well-behaved-client model. Watch; revisit if reinit escalation
    misfires appear.
  - **B2** — cap-plane transient where a call between a provider's sweep and its
    fresh re-registration gets `EUNKNOWN`; benign (fail-soft, caller retries), and
    RT5's quiesce narrows the create/remove flavour of it. Left as-is.
  - **F2** — a second, lower-severity fault-attribution corner in the funnel's
    crash path; subsumed by RT6's gate (the handler no longer races a destroy) and
    not separately reproducible. Re-check after RT6 review.

- **REFUTED (looked suspect, verified safe):**
  - Hot-reload racing an in-flight inspect — **safe**: `run_inspection_compute_`
    snapshots the `LoadedScript` under `script_mu` and `module_lifetime` defers
    `FreeLibrary` + owner-sweep until the last in-flight copy drops
    (`xi_script_loader.hpp`); lifecycle ops quiesce and drain first.
  - `instance_group` read racing create/remove — **safe**: already taken under
    `mu_` (the fix that landed with the F1 groups round-trip).
  - EmitTurn dtor backstop stranding a lane on early-return/throw — **safe**:
    covered by `test_emit_gate`'s dtor-backstop + stop-wake sections; the dtor
    takes the turn in order on every exit.

---

## Test map (one line each)

| Finding | Regression test |
|---|---|
| RT1 / F1 | `plugins/use_pack_door_test.cpp` §8b (stream identity survives a hop) |
| RT2 / F3 | `examples/qa_pack_stream` (`s4_foreign_fault_ignored`, driver-asserted) |
| RT3 / F5 | `plugins/record_replay_pack_test.cpp` §G (no `$channel/$seq` injected) |
| RT4 / B1 | `backend/tests/test_cap_provider_refcount.cpp` (shadow promotion) |
| RT5 | `examples/qa_remove_under_load/` (remove under continuous load) |
| RT6 / A1·A2 | `backend/tests/test_cap_reinit_race.cpp` (funnel vs reinit) |
| RT7 / P1 | `backend/tests/test_emit_gate.cpp` §P1 (verdict⇔actuation coupling) |
| P2 | design note only (this doc §P2 / doc 19 RT8) |
