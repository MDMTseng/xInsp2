# Core simplification — patch-necessity archaeology

> **Status:** analysis-only (nothing here is implemented). Product of the 2026-07-10
> essential-vs-accidental complexity audit across five core patch families. Grounded
> in spine principle #2 (**minimal core**): the core does only dispatch, lifecycle,
> crash-safety, refcounted zero-copy pools, and the frozen ABI — everything else is a
> plugin. The goal is fewer branches + smaller vuln surface + easier analysis, with
> **zero or near-zero change to the external interface** (WS command surface + frozen
> `xi_pack_v1`/image-handle ABI). Speed is a *secondary* and mostly *modest* effect —
> see the honest accounting at the end.

## The one thesis

The core *requirement* is simple. The complexity is accreted **patches for corner
cases**, and — this is the pattern across all five families — the recurring bug/patch
classes exist because a correctness ritual lives as a **per-call-site convention**
(enforced by review + memory) instead of a **type or structure** (enforced by the
compiler or by construction). The codebase has *already invented the correct
structural form* several times and simply never generalized it:

- `InflightRuns::launch()` turned "bump-then-check at every launch site" (a repeatedly-
  missed UAF) into one primitive — its header says the missed bump *was* the UAF.
- The A4 explicit `XI_INSPECT_ENTRY(t, frame)` turned the trigger (the largest per-run
  state) into a self-contained object "valid on any thread" — retiring ambient TLS *for
  the trigger only*.
- `xi_script_loader`'s `module_lifetime` refcount solved unload-vs-inflight **structurally**
  for the *script* DLL (a missed guard degrades to a deferred unload, never a UAF).
- `ImagePoolOwnerScope` consolidated the failed-ctor sweep that 4+ sites hand-rolled.

Each family below is "the same move, not yet made everywhere."

## The five families

| Family | Essential core | The accretion | Top interface-preserving simplification |
|---|---|---|---|
| **Live lifecycle vs dispatch** | quiesce-then-mutate-then-resume; evict-before-unload ordering | the quiesce/evict ritual is opt-in per site → RT5/J2/J3/G2/L1/O2/N1 = "op X forgot the ritual" (~9 defects / ~16 sites) | **QuiesceToken capability**: destructive `PluginManager` ops require a token only `DispatchPoolGuard` can mint → forgetting = compile error; `dismiss()`→`skip_resume()`; one `unload_live_module_()`; ban bare `FreeLibrary` |
| **Pack container / codec** | two storage planes (arena + pool); canonical-at-edge ingress; `$fault` short-circuit | TypedPack (2nd container, **0 prod consumers**, ~425 LOC); `pack_mp_detail` (2nd encoder, "SWAP TARGET" never swapped, already NaN-divergent); `$prov` chain stamped every hop/frame | **Delete TypedPack + fold `pack_mp_detail`** (~500 LOC, −40% of the container header, closes NaN bug); `$prov` chain fault-path-only; `is_reserved()` for 5 duplicated `$`-filters |
| **Ambient thread_local context** | owner + cancel-token are allocator/poll-shaped (can't reach opaque callees) | run_id/frame_path/result/staging left on ambient TLS after A4 moved the trigger → F4/J4/U3/silent-result guards + per-primitive re-install Scopes; `spawn_worker` forgot all of them | **Finish A4**: put run_id/frame_path/result-slot on `xi_trigger_view`→`Trigger`; free functions become shims over one macro-installed entry-thread ctx (null off-thread ⇒ one fail-loud). Retires 5 of 9 TLS, both guard families, `TriggerCtxScope`, the spawn_worker gap |
| **Owner-tag / sweep** | image tag-once + cold sweep (near-free, minimal); failed-ctor scope; owner-0 | destructible singletons → alive-flag guards; PackRegistry **counted per-owner ledger** (R1 clamp + tagged/untagged split) = "most bug-productive structure", ≥3 UAF/leak classes; instance tag on pack-interior buffers (the cross-plane free) | **Owner-0 mint pack buffers** (= the A1 fix on this branch); **collapse the pack ledger to a single creator tag** (image-pool model, fail-closed to leak) → deletes R1 clamp/`ledger_*`/`OwnerRef`/untagged split (~110-130 LOC, 0 ABI); **leak the process-lifetime singletons** → deletes 3 alive-flags + a static-destruction-UB class |
| **Cancel / watchdog / ordering** | hard-trip `_Exit`+respawn (only reliable wedge recovery); per-task `CancelToken`; the emit gate (pay-per-use, load-bearing for the PLC frame-order contract) | the **cooperative-cancel layer** — the grace window already spares merely-slow frames *without polling*; cooperative cancel only helps frames >budget+1s that also poll (near-empty), yet required the whole ticket epoch + ticket-0 landmine + spared/targeted machinery + G3 + the still-open targeted wrong-PASS class | **Retire cooperative cancel; keep hard-trip + tokens** → watchdog collapses to one phase; `cancellation_requested()` stays token-only (no API break); ~150 LOC + 2 vuln classes gone. Ordering: keep the gate, just flatten the arrival_id/eseq/run_id triple |

## Ranked roadmap (payoff ÷ interface-disruption)

**Tier 0 — do now, tiny + already-proven (zero interface):**
1. **Owner-0 mint pack-interior buffers** — 2 lines; kills the cross-plane sweep UAF class. *(Already implemented as the A1 fix on `fix/redteam-round11`; port/keep it.)*
2. **Leak the process-lifetime singletons** (ImagePool/PackRegistry/cap registry) — deletes 3 alive-flag guards + the static-destruction-UB class, ~30 LOC + 3 atomics.
3. **`is_reserved()` helper + dequeue `run_id` fallback removal** — collapse 5 duplicated `$`-filters; drop the dead `++g_eng.run_id` fallback.

**Tier 1 — high leverage, structural, zero external change:**
4. **QuiesceToken capability + one `unload_live_module_()` + `skip_resume()`** — converts the ~9-incident live-teardown family from recurring review finding to compile error. Same pattern as `InflightRuns::launch`.
5. **Delete TypedPack + fold `pack_mp_detail`** — ~500 LOC, one container/one encoder to audit, closes the NaN divergence. Nothing in production reaches TypedPack across the opaque-handle ABI.
6. **Collapse the PackRegistry ledger to a single creator tag** — deletes the R1 clamp + `ledger_*` + `OwnerRef` + tagged/untagged split; the two nastiest historical UAF mechanisms become unrepresentable. (Consumer-retain leaks downgrade from swept to *diagnosed* + teardown-reclaimed — needs a hot-reload soak test.)

**Tier 2 — larger, still interface-preserving (bilingual-window migration):**
7. **Finish A4: explicit per-run context** (run_id/frame_path/result onto the trigger; free functions become shims; cut the legacy ambient-trigger path). Retires the whole F4/J4/U3/silent-result family + the `spawn_worker` gap. Fold owner+cancel into one `PropagatedContext`. The only "break" is scripts that already misuse free-functions inside async bodies — they change from silent-wrong to fail-loud (a fix).
8. **Retire the cooperative-cancel layer** (keep hard-trip + tokens; one-phase watchdog). Flag to the app team: a >1s-over-budget *polling* script now respawns instead of soft-aborting. Note this also retires the need for the C1 verdict-safety patch (the wrong-PASS class it guards vanishes when a frame either completes or the process dies).

## Essential — do NOT touch

Image pool tag-once + cold `release_all_for` spare-don't-force-delete (M2); `ImagePoolOwnerScope` (failed-ctor, no other releaser possible); owner-0 anonymous escape; `WalkGuard` deferred reclamation (a stats-walk fix); the two Pack storage planes + `ArenaPool` freelist; canonical-at-edge ingress (genuinely does NOT leak onto the trusted path); `$fault` short-circuit + mint-on-fault (forced by pack immutability); watchdog hard-trip; the emit gate + `StagedEmitGuard` crash-atomicity (staging is *crash-atomicity*, not just ordering — needed in completion mode too); per-task `CancelToken`; the frozen `xi_pack_v1` / image-handle ABI.

## Honest payoff accounting

- **Speed:** mostly *not* where the cycles are. The image hot path already carries zero per-addref/release owner cost; the ledger scans are per-frame under an already-held lock. The *real* per-frame speed wins are narrow and specific: **PackRegistry takes a global mutex per ABI key-read** (a plugin reading 10 keys locks 10× — dwarfs the 273 ns TypedPack "saves"; fix = `shared_mutex` or resolve-handle-once-per-door, interface-preserving); **`$prov` chain stamping** taxes every healthy frame at every hop; the TLS thunk reads at each async spawn. Everything else is single-digit-cycle.
- **Vuln surface (the real prize):** these safety nets have themselves produced the majority of the red-team findings — the ledger alone caused ≥3 UAF/leak classes; the ambient-TLS pattern caused F4/G3/J4 + the spawn_worker gap; the per-site quiesce ritual caused ~9 teardown UAFs; cooperative cancel left the wrong-PASS class open. Each structural simplification eliminates a *class*, not an instance.
- **Analyzability:** "can dispatch race a teardown?" goes from auditing 13+1+17+3+2 sites to 3 functions + compiler enforcement. "Where does per-run identity come from?" goes from 9 TLS + N re-install sites to one struct field. "Can the sweep over-free?" becomes structurally answerable.

## Meta-recommendation

Prefer the moves that convert a **convention into a type/structure** (QuiesceToken, explicit context, single creator tag, leaked singletons) over the moves that merely delete code (TypedPack) — the former retire the *recurrence*, which is where this core actually bleeds. Every one of these has an existing in-repo precedent proving the pattern ends a bug family for good.
