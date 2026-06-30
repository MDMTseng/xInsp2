# Open Questions — deferred decisions (autonomous execution log)

**Context:** User stepped away 2026-07-01 mid-roadmap (Parts I–IV of `core_fix_plan.md`).
Standing instruction: log deferrable decisions here, proceed on the recorded default, only
stop and ask if genuinely blocked. Each item records **the default I am proceeding with**,
**why it is safe to defer**, and **what would flip it**. Resolve with the user on return.

---

## OQ-1 — Race detection on Windows (MSVC has no ThreadSanitizer)
- **CONFIRMED (2026-07-01, Tier 0 T0.1):** MSVC 19.50 has **no TSan and no UBSan**; **clang-cl is not
  installed** in this VS (only clang-format/clang-tidy ship). So on this machine, real TSan is not available.
- **Implemented default:** **ASan works and is live** (`-DXINSP2_SANITIZE=address`, verified to trap a real
  heap-overflow). For races: `XINSP2_STRESS_SCALE` high-iteration ctests (`image_pool_stress_heavy`,
  `set_def_race_heavy`) runnable **under ASan** to catch the UAF/heap-corruption class that races produce.
  `-DXINSP2_SANITIZE=undefined|thread` hard-error honestly rather than pretending.
- **STILL OPEN (for user):** real data-race detection (TSan) needs either **(a) a Linux CI lane**
  (repo has `docs/roadmap/linux-port.md` + TODO(linux) markers) or **(b) installing LLVM/clang-cl** on the
  Windows build box for a `-fsanitize=thread` config. Proceeding without it; Part III G2 (T1.3) race
  verification will lean on ASan + stress-scale until one of (a)/(b) is chosen.
- **Flips if:** user stands up a Linux CI lane or installs clang-cl/LLVM.
- **Impacts:** Tier 0 T0.1 (done, ASan); Tier 1 T1.3 (TSan stress on Part III G2 — degraded to ASan+stress).
- **WIDER CONSEQUENCE (confirmed Phase 2):** the **same** missing-clang also blocks **Tier 1 T1.1/T1.2
  in-process libFuzzer** (`-fsanitize=fuzzer` needs clang/clang-cl). So the genuine Cluster-2 gap —
  coverage-guided in-proc fuzz of `parse_cmd`/yyjson/ABI/`get_interface` — **cannot be closed on this box**.
  It is gated on the same (a) Linux CI lane or (b) install LLVM decision. Until then, in-proc parsers are
  covered only by the black-box WS smoke (Tier 0 T0.2), which cannot reach the in-proc C-ABI `get_interface`.

## OQ-2 — Push to origin vs stay local
- **Default:** keep all completed work **local on `master`** (now ahead of `origin/master`); do **not** push.
- **Why deferrable:** no instruction to push; nothing here is outward-facing yet.
- **Flips if:** user wants remote review / CI → push feature branches or open PRs.

## OQ-3 — A1 off-thread fail-loud has no integration test
- **Default:** leave as a documented integration-level follow-up. A2/B/C are unit-covered
  (`test_parallel_safety`); A1's `g_inspect_tid` abort/log path lives in `service_main.cpp` and only
  fires with the real dispatch worker + `CurrentTriggerScope`.
- **Why deferrable:** the hazard it guards is now *detectable*; the test is belt-and-suspenders.
- **Flips if:** user wants it covered before further structural work → add a dispatch-level integration test.

## OQ-4 — Part III G1 certify host: separate exe vs runner mode
- **Default:** a `--certify-plugin <dir>` **mode of an existing binary** (runner_main or a tiny sidecar),
  child-process load+factory-call, verdict cached by DLL content-hash. Cheapest; reuses `xi_crash_dump`.
- **Why deferrable:** an implementation shape, not a user-facing contract.
- **Flips if:** user wants a dedicated cert service / different isolation model.

## OQ-5 — Part III G4 state-migration hook vs drop-on-mismatch
- **Default:** keep current **drop-on-mismatch**; do NOT add a `code_change`-style migrate hook now
  (Part III §18 G4 "only if needed").
- **Flips if:** cross-version state continuity becomes a stated requirement.

## OQ-6 — Plugin trust / signing (deliberate non-goal)
- **Default:** keep "plugins are trusted" (`xi_plugin_manager.hpp:1150`, cert gate removed 2026-06).
  Part III G1 subprocess-certify addresses **crash safety**, NOT supply-chain trust — they are separate.
- **Flips if:** distribution of third-party *untrusted* plugins becomes real → revisit signing (Sigstore/Authenticode).

## OQ-7 — Part IV partial gaps (lower priority, backlogged)
- **Items:** observability export (OpenTelemetry/metrics/latency histograms), WS protocol version
  *negotiation* (today: reports version, no gate), Record cross-plugin *static* schema contract.
- **Default:** backlog after the three correctness nets (Part IV §27.5). Do not gold-plate.
- **Flips if:** a concrete need appears (e.g. production-line monitoring requirement, a client-compat break).

## OQ-8 — perf gate is sensitive to the JPEG encoder backend (Tier 0 T0.3 follow-up)
- **Observed (Phase 2, 2026-07-01):** `perf_jpeg`'s baseline (2745 µs) was captured with **libjpeg-turbo/IPP** detected.
  A **fresh agent worktree** whose CMake configure does *not* detect turbojpeg falls back to the stb encoder
  (~17 ms) → the gate trips as a **false positive**. Confirmed harmless: two independent Phase 2 tracks both
  tripped `perf_jpeg` on code paths neither touched; it passes on the master build dir (turbojpeg present).
- **Precise cause (confirmed Phase 3):** a fresh worktree configures with **`XINSP2_HAS_TURBOJPEG=OFF`** (and
  `XINSP2_HAS_IPP=OFF`) by default → bench_jpeg uses the OpenCV/stb encoder (~17 ms) vs the libjpeg-turbo
  baseline (2.75 ms), tripping the gate ~+511%.
- **Default proceeding:** treat a `perf_jpeg` red in a *fresh-worktree* build as a config artifact; **build each
  gate with `-DXINSP2_HAS_TURBOJPEG=ON -DXINSP2_HAS_IPP=ON`** (the canonical/master config). Done for Gate C/D.
- **Follow-up fix (small, backlog):** record the encoder backend in the baseline file and have `perf_gate.cmake`
  **skip-with-warning** (not fail) when the measured backend differs from the baseline's; or pin the backend in CI.
- **Flips if:** user wants the gate hardened now rather than backlogged.

---

### Status snapshot (end of autonomous run, 2026-07-01)
- **Part II Phase 4 (retire monolith) precondition is now MET:** Track B3 confirmed `expose` was the **only**
  first-party plugin calling a raw carved host field; it's migrated to the SDK wrapper, and all other plugins
  already use SDK helpers. So Phase 4's "all first-party plugins use interfaces" gate is satisfied. **The sole
  remaining Phase 4 blocker is removing the dead `shm_*` stubs from `xi_host_api`, which is an ABI MAJOR break
  (not additive) — needs user sign-off** (a v11 with min-compat raised, dropping pre-v6 plugins).
- **Part III G3.2 (debug illegal-transition asserts)** deferred to backlog (A3 kept the handshake PR focused).
  The contract is documented (G3.1); the asserts that enforce it are the open follow-up.

### Resolved (recorded for trail)
- **II-vs-III ordering** → **parallel tracks** after Tier 0 (user: "能平行就平行", 2026-07-01).
- **Gated pipeline vs single auto-workflow** → **gated, phase-by-phase** with build+test gates (user-approved).
- **compress / log / status pluginization** → **NO**, stay ambient host_api (Part II §9; ambient-vs-composable).
- **replay** → cache-plugin's job, can become an official plugin; **not** a core concern (user, 2026-06-30).
