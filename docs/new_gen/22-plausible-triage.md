# 22 — PLAUSIBLE-finding triage (RT8)

Branch: `polaris2/rt8-plausible-tests` (off `origin/polaris2_main` @ `45dc6da`).

The v12 red-team pass (doc 21) left a handful of findings rated **PLAUSIBLE** —
not proven, not refuted. This doc closes each one: a focused stress/concurrency
test that tries to REPRODUCE it under heavy load with **well-behaved clients**
(no malformed input), then a verdict — **CONFIRMED-with-repro** or **REFUTED**
with the code line that makes it impossible. No finding is left ambiguous.

Threat model (unchanged): well-behaved clients under heavy load; the timing/
contention corners that only show up at scale.

Legend: **CONFIRMED** = a test reproduces the defect · **CONFIRMED-benign** = the
window/behaviour is real and reproduced, but fail-soft by contract (no
correctness or memory-safety hazard) · **REFUTED** = an invariant makes it
impossible; test (if any) deleted.

---

## Summary table

| # | Finding | Verdict | Test |
|---|---|---|---|
| F2 | `$prov` unbounded growth | **REFUTED** | none (code-cited) |
| R1 | PackRegistry owner-ledger mis-attribution → over-release | **CONFIRMED → FIXED** | `backend/tests/test_pack_ledger_misattribution.cpp` |
| A3 | cap-plane spurious `-4` (ESHAPE) during provider swap | **CONFIRMED-benign** | `backend/tests/test_cap_reload_window.cpp` |
| B2 | autoload-swap unavailability window (`-1` EUNKNOWN) | **CONFIRMED-benign** | `backend/tests/test_cap_reload_window.cpp` |
| 5th | doc-21 "F2" funnel crash-path fault-attribution | **REFUTED** (subsumed by RT6) | none |

Net: **one real latent defect (R1) — now FIXED with regression**; two
real-but-benign fail-soft windows (A3/B2, quantified); two refutations.

---

## F2 — `$prov` unbounded growth · REFUTED

**Claim.** The pack provenance hop-chain `$prov`
(`backend/include/xi/xi_pack_contract.hpp` `kProv`, `prov_append`) can grow
without bound; suspected reachable only via a pathological loop-forward script.

**What `$prov` actually is.** One hop is appended per door pass:

- door output — `xi_abi.hpp:554`
  `out.prov(prov_append(prov_parent(fi, in), name_))`
- fault short-circuit — `xi_pack_contract.hpp:160`
  `prov_append(prov_parent(fi, in), hop_sv)`

`prov_append` (`xi_pack_contract.hpp:106`) just does `parent + '/' + hop`. There
is **no cap** — correct: the length is bounded by the *pipeline depth*, not by a
counter. A frame flows through a finite chain of doors exactly once, so
`len($prov) ≈ Σ hop-name lengths` over the pipeline — kilobytes at worst for an
absurdly deep pipeline, and *constant per frame* (it does not accumulate across
frames — each new frame/stream-chunk starts a fresh chain from its source).

**Why a cycle can't pump it.** The only way to make `$prov` grow without bound is
to feed a pack's own output back to its own input in a loop. Both funnel
directions refuse that **before any work**, per-thread:

- `xi_cap_abi.hpp:301` — `if (e.owner == cur || cap_stack_contains(e.owner))
  return XI_CAP_EREENTRY;` (a call into an instance already on this thread's
  funnel stack, or the caller's own instance, is `-5`).
- `xi_use.hpp` §sink guard — `process()` onto a declared ordered sink / self is
  `-5` (`warn_use_sink_target_`).

So an *automatic* synchronous cycle (A→B→A, or A→A) is impossible: the chain
depth on any one thread is strictly bounded by the acyclic call graph.

**The only residual growth path is out-of-model.** A plugin could read `$prov` as
a string, STORE it across frames, and re-inject it into a fresh pack every frame
— an ever-growing string. That is the plugin *authoring* unbounded growth by
hand (it must copy the chain forward itself; the core never does across frames),
which is plugin misbehaviour, not a core defect, and outside the well-behaved
threat model. The reentrancy guard the finding suspected is indeed the block for
every *automatic* case.

**Verdict: REFUTED.** No cap is needed because the chain is pipeline-depth
bounded and per-frame; cycles are refused `-5`; cross-frame accumulation requires
a plugin to deliberately re-inject the chain. No test kept.

---

## R1 — PackRegistry owner-ledger over-release · CONFIRMED → FIXED

**Where.** `backend/include/xi/xi_pack_abi.hpp` — `PackRegistry` owner ledger
(`ledger_release`, `ledger_take`, `release_all_for`).

**The stated invariant this breaks.** The header comment (lines ~70-73, ~220-226)
claims: *"A release under the wrong/no owner context is reconciled against the
untagged headroom first, then against any ledger bucket, so the ledger can never
claim more refs than the slot holds and **a sweep can never over-release.**"*

**The corner where it fails.** `ledger_release(s, owner=0)` (a **guardless**
release — `ImagePool::current_owner()==0`) tries untagged headroom first:

```cpp
int sum = 0; for (const OwnerRef& r : s.owners) sum += r.n;
if (sum < s.rc) return;              // untagged headroom absorbs it
if (!s.owners.empty()) {             // else: mis-charge the LAST bucket
    if (--s.owners.back().n == 0) s.owners.pop_back();
}
```

When a pack is co-held by **two tagged owners with NO untagged headroom**
(`sum == rc`), a guardless release decrements `owners.back()` — an owner that did
NOT release. `rc` still drops correctly, but the ledger now mis-attributes:
it believes the *other* bucket is the live holder. When that mis-charged-FROM
owner's adapter is torn down, `release_all_for(owner)` takes its **phantom
bucket** and drives `rc` to 0 — **freeing the pack while the true co-owner still
holds a ref.** Over-release / latent UAF.

**Reachability (well-behaved, heavy load).** A reentrant provider contracts to be
thread-safe and the plane runs its handler off many dispatch threads
(`xi_cap_abi.hpp` header). A provider that retains a shared pack under its
`OwnerGuard` (tagged) but **releases it from a worker thread it spawned** (off any
guard → `owner==0`), while a second instance co-holds the same pack and the emit
event's untagged ref has already drained, hits exactly this corner. RT5's quiesce
removed the *remove-instance* flavour of the guardless release, not the
plugin-worker flavour.

**Repro + regression.** `backend/tests/test_pack_ledger_misattribution.cpp`
drives the exact sequence through the public `PackRegistry` API
(`retain_as`/`release_as`/`release_all_for`/`owner_refs`/`pack`) and pins the
safety invariant across four sections: §1 over-release (the co-owned pack must
SURVIVE the sweep — the key assertion, which the pre-fix ledger violated by
freeing under B), §2 double-free (redundant sweep + a release on the freed handle
are no-ops), §3 leak/defer (the deferred free still happens on B's true last
release → count reaches 0), §4 leak/reclaim (a genuinely forgotten single-owner
ref is still reclaimed by its sweep). On the pre-fix ledger §1 FAILS; on the
fixed ledger all sections pass. Normal PASSING ctest target.

**Fix (LANDED — `xi_pack_abi.hpp`).** Two coordinated changes; the naïve
sweep-only guard is insufficient because the mis-attribution already *erased* the
co-owner's bucket before the sweep ran.

1. **`ledger_release`** — an unattributable release (owner 0, or a tagged owner
   with no matching bucket) **never touches a bucket**. The prior code, once the
   untagged headroom `rc - sum(buckets)` was exhausted, decremented
   `owners.back()` — popping a *live co-owner's* bucket. That guess is deleted;
   `rc` alone (decremented in `release_as`) records the drop, so a co-owner's
   bucket is preserved.
2. **`release_all_for` (the sweep)** — reclaim only the refs **not attributable
   to a surviving owner**, and **never free while another owner's bucket is
   non-empty**:

   ```cpp
   int k = ledger_take(s, owner);          // remove this owner's bucket
   if (k > 0) {
       int remaining = 0;                  // survivors' (max) holds
       for (const OwnerRef& r : s.owners) remaining += r.n;
       int reclaim = s.rc - remaining;     // refs beyond any survivor = this owner's
       if (reclaim < 0) reclaim = 0;
       if (reclaim > k) reclaim = k;       // never exceed this owner's own bucket
       swept += reclaim;
       s.rc -= reclaim;
       if (s.rc <= 0 && s.owners.empty())  // free ONLY when no ref & no survivor
           { /* drop + erase */ }
   }
   ```

**Fail-closed reasoning.**
- *Over-release (UAF) — impossible.* The sweep frees only when `rc<=0 && owners
  empty`; a live co-owner keeps a non-empty bucket, so its pack is never freed by
  another owner's sweep. `reclaim = rc - remaining` is clamped `≥0`, so `rc` never
  drops below `remaining` (the survivors' holds). The true last holder frees it on
  its own release (`release_as`: `if (--rc==0) erase`, rc-authoritative).
- *Leak — the free still happens.* `rc` is the source of truth and is decremented
  on **every** release (tagged, untagged, or off-guard), so any pack whose holders
  all eventually release reaches `rc==0` and frees — even a stale bucket dies with
  the frame (§3 pins this). A genuinely forgotten single-owner ref is reclaimed by
  its sweep (`remaining==0 → reclaim==rc`, §4 pins this).
- *ABI unchanged.* Header-internal `PackRegistry::Slot` logic only; no field
  added, no struct in `xi_abi.h` touched. `test_abi_freeze` stays green at
  **112 bytes / 14 fields** (`XI_ABI_EXPECTED_SIZE` unchanged).

**Residual (flagged, strictly better than the UAF).** One narrow *double-fault*
is fail-closed toward a **bounded leak**, never a UAF: if a surviving owner's
bucket is *stale* (that owner released off-guard with no headroom, so its bucket
lingers) **and** the swept owner had a genuinely forgotten ref, the stale bucket
inflates `remaining` and the sweep defers that forgotten ref's reclaim — the pack
lingers until registry teardown. This requires two simultaneous faults
(off-guard release **and** a forgotten-ref sweep on the same co-owned pack); the
pre-fix code handled *this* particular case but UAF'd the mirror case. A fully
leak-free fix needs off-guard releases eliminated at the source (release under the
same owner context the ref was acquired) — an adapter/discipline change, not a
header-local one. For a WELL-BEHAVED caller (releases under its guard) neither the
UAF nor the residual leak is reachable, and observable single-owner ledger
semantics are unchanged.

---

## A3 — cap-plane spurious `-4` during provider swap · CONFIRMED-benign
## B2 — autoload-swap unavailability window (`-1`) · CONFIRMED-benign

**Where.** `backend/include/xi/xi_cap_abi.hpp` funnel (`f_cap_call`,
`resolve_provider_`) vs `backend/include/xi/xi_pm_load.hpp`
`evict_machine_provider_locked_` + `reload_machine_provider`.

**The windows.** `reload_machine_provider` evicts then re-creates under the PM
lock; a concurrent `f_cap_call` takes **no PM lock**, so `evict`'s ordering opens
two windows:

```cpp
InstanceRegistry::instance().remove(name);   // (1) out of the live list
machine_instances_.erase(it);                 // (2) dtor -> sweep_caps_for
... autoload re-creates ...                   // (3) fresh factory re-registers
```

- between (1) and (2): `CapRegistry::lookup` still finds the name, but
  `resolve_provider_` no longer finds the adapter → **`XI_CAP_ESHAPE` (-4)** — the
  spurious `-4` of **A3** (`f_cap_call` line 281).
- between (2) and (3): the name is swept, `lookup` misses → **`XI_CAP_EUNKNOWN`
  (-1)** — the unavailability window of **B2** (`f_cap_call` line 277).

**Repro + measurement.** `backend/tests/test_cap_reload_window.cpp` — 6 reader
threads hammer `test.echo` while a writer runs 300 evict+re-create swaps (the
reload shape, fresh owner id each time). Observed on this machine:

```
swaps=300 ok=4868  transient: EUNKNOWN(-1)=263 ESHAPE(-4)=2382 EQUAR(-3)=0
          other=0  wrong_answers=0
```

Both windows are **real and readily hit under load** (A3's `-4` especially —
2382 hits). Crucially: **`wrong_answers=0` and `other=0`** — every resolved
handler returned a coherent answer (RT6's reinit-gate + re-resolve-under-lock
guarantees no freed `inst_` is ever entered), and only the two documented
fail-soft codes appeared. No crash, and the pack table balances at teardown.

**Verdict: CONFIRMED-benign.** The windows exist (quantified above) but are
fail-soft on a **caller-retry contract** (doc 21 already classifies both as
benign): a caller that retries the transient succeeds against the reinstated
provider. No memory-safety or correctness hazard. The test stays GREEN — it
asserts survival + coherence + eventual correctness, **not** zero transients.

**Optional hardening (NOT landed).** If the `-4`/`-1` blips ever matter to a
latency-sensitive consumer, `reload_machine_provider` could **re-register the
fresh provider before evicting the old** (register-then-swap) so the name is
continuously resolvable, or the funnel could retry once internally across a
sweep gap. Neither is warranted by the current benign contract; recorded for the
app team.

---

## 5th finding — doc-21 "F2" funnel crash-path fault-attribution · REFUTED

Doc 21's "PLAUSIBLE, not reproduced" list has a *second* item it labels **F2** (a
fault-attribution corner in the funnel's crash path — distinct from the `$prov`
F2 above). Per doc 21 it is *"subsumed by RT6's gate (the handler no longer races
a destroy) and not separately reproducible."* RT6's reinit-gate
(`xi_cap_abi.hpp:319`, shared-lock + re-resolve) is in place on this tip: the
crash path (`catch (seh_exception)`) charges `apply_on_fault_` to a live,
gate-pinned adapter, and the handler cannot run against a freed `inst_`. No
separate reproduction exists once RT6 is present. **REFUTED (subsumed).**

Conclusion on the "5th": there are **four** genuinely distinct PLAUSIBLE findings
(F2-`$prov`, R1, A3, B2); the fifth candidate is the doc-21 crash-path
fault-attribution corner, which RT6 already closed.
