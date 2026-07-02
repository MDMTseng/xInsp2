# Open Questions — deferred decisions (autonomous execution log)

> **Status:** experimental — deferred decisions with recorded working defaults, not
> ratified design. Each item's default is subject to change on review.
> **Last verified against:** 2026-07-01 (`core_fix_plan.md` Parts I–IV in progress).

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
- **Race-hotspot inventory (what to test + gaps):** see `../archive/core_fix_plan-2026-07.md` §28 (the plan is now implemented + archived; residue in `core_fix_plan.md`) — the concrete surfaces (image
  pool, DocRegistry/COW, dispatch queue+EmitGate, set_def/prepare vs process, hot-reload swap, culprit stamp,
  owner propagation, watchdog, status), which have tests vs the GAPS, and the two actions (Windows gap-fills
  now; Linux TSan lane later).
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

## OQ-9 — Decouple OpenCV from the mandatory plugin surface + JPEG backend strategy (user raised, 2026-07-01)
- **Source-verified facts:** the **kernel is already OpenCV-free** (`xi_image_pool`/`xi_record`/`xi_doc_registry`/
  `xi_trigger_bus` = 0 cv refs); `xi_abi.hpp`'s "3 cv refs" are all **comments** (example code); **stb is the
  zero-dependency unconditional JPEG fallback** (`encode_jpeg` ends `return encode_jpeg_stb(...)`; turbojpeg +
  OpenCV are optional `#ifdef` fast paths); **`xi_cv.hpp` is already a standalone opt-in convenience header.**
  The ONLY hard coupling: (1) `xi.hpp` umbrella unconditionally `#include <opencv2/opencv.hpp>`, and (2)
  `xi_image.hpp`'s `as_cv_mat()`/`from_cv_mat()` need cv types.
- **Q-B — does core need OpenCV? NO.** OpenCV is a plugin-author convenience force-fed via the umbrella.
  **Recommended cut (seam already exists):** move `as_cv_mat`/`from_cv_mat` from `xi_image.hpp` → `xi_cv.hpp`
  (opt-in); drop the opencv include from `xi.hpp`. Then a plugin that does no CV builds with **zero OpenCV**.
  Breaking-ish (cv-using plugins add `#include <xi/xi_cv.hpp>`) but in-tree plugins are rebuildable.
- **Q-A — drop turbojpeg, rely on OpenCV? NOT recommended** — that *deepens* the OpenCV coupling Q-B wants to
  shed, and costs ~6× JPEG encode (2.7 ms→~17 ms here). The dependency-minimal JPEG path is **stb** (already
  present). Keep turbojpeg as a lean, focused **optional fast path**; make OpenCV **optional as a backend**, not
  required. i.e. stb default → turbojpeg optional accel → OpenCV optional.
- **DONE (2026-07-01, on v11):** `as_cv_mat`/`from_cv_mat` moved to opt-in `xi_cv.hpp` as free functions;
  `xi.hpp` + `xi_image.hpp` are OpenCV-free; 21 call-sites migrated; `find_package(OpenCV)` dropped from
  `plugins/CMakeLists.txt`. **Proof:** all 10 in-tree plugin DLLs build with **OpenCV absent** (plugins
  CMakeCache has zero OpenCV); backend 31/31 ctest green; freeze guard unchanged. turbojpeg **kept** (only
  real codec caller is `xi_jpeg.hpp`; the rest is optional toolchain plumbing — see below).
- **Process note:** the OQ-9 agent branched off a stale `1bdc092`/v9 base and didn't re-root — its clean 30
  files were taken verbatim onto v11, the 4 conflict files (all comment/one-line-include) hand-merged, then
  re-verified on v11. Lesson: agent worktrees can seed off an old base; always confirm ABI v11 / 31 tests
  before trusting an agent's "green".
- **Still open (turbojpeg removal):** NOT recommended (kept). If ever removed: codec is 1 `#ifdef` branch in
  `xi_jpeg.hpp`; plumbing spans ~8 files (`CMakeLists.txt`, `xi_script_compiler.hpp`, `xi_plugin_manager.hpp`,
  `xi_project_model.hpp`, `service_main.cpp` toolchain block, `runner_main.cpp`, `xi_plugin_export.hpp`). OFF
  by default → zero footprint when unused.

---

### Status snapshot (end of autonomous run, 2026-07-01)
- **Part II Phase 4 (retire monolith) precondition is now MET:** Track B3 confirmed `expose` was the **only**
  first-party plugin calling a raw carved host field; it's migrated to the SDK wrapper, and all other plugins
  already use SDK helpers. So Phase 4's "all first-party plugins use interfaces" gate is satisfied. **The sole
  remaining Phase 4 blocker is removing the dead `shm_*` stubs from `xi_host_api`, which is an ABI MAJOR break
  (not additive) — needs user sign-off** (a v11 with min-compat raised, dropping pre-v6 plugins).
- **Part III G3.2 (debug illegal-transition asserts)** deferred to backlog (A3 kept the handshake PR focused).
  The contract is documented (G3.1); the asserts that enforce it are the open follow-up.

### Decisions made (user returned, 2026-07-01)
- **OQ-1 → option (b): install clang-cl.** Wire a clang-cl build config → **in-process libFuzzer** targets
  (`parse_cmd`/yyjson/ABI/`get_interface`) + **UBSan**. (TSan stays unavailable — clang-cl TSan on Windows is
  not reliable; real TSan remains a future Linux-lane item, not blocking.)
- **Phase 4 → GO, full "大破大立" in one shot. Major ABI break authorized.** Stop publishing `xi.legacy`,
  **remove the dead `shm_*` stubs** from `xi_host_api`, bump `XI_ABI_VERSION`→11, raise `XI_ABI_MIN_COMPAT`,
  evolve the freeze guard to v11, rebuild all in-tree plugins. Old-layout plugins refused (acceptable — all
  first-party plugins are rebuildable in-tree).
- **OQ-2 → push as a BRANCH** (not master). Open for review; do not move `origin/master`.
  **DONE (2026-07-01):** pushed `origin/review/core-roadmap-v11-2026-07` (master's full lead: v11 大破大立, clang-cl libFuzzer/UBSan, OQ-9 OpenCV decouple, §28 race nets).
  **SUPERSEDED (2026-07-01, user):** "you merge it to master" → fast-forwarded `origin/master` `1bdc092 → e9be4b6`. `origin/master` now carries the whole body of work; the review branch is a redundant pointer to the same commit (kept for now, safe to delete).
- **All other OQs (OQ-3, OQ-5, OQ-6, OQ-7, OQ-8, G3.2) → keep documented, user decides later.**
- **OQ-4** → already implemented (runner `--certify-plugin` mode); effectively closed.

### Resolved (recorded for trail)
- **II-vs-III ordering** → **parallel tracks** after Tier 0 (user: "能平行就平行", 2026-07-01).
- **Gated pipeline vs single auto-workflow** → **gated, phase-by-phase** with build+test gates (user-approved).
- **compress / log / status pluginization** → **NO**, stay ambient host_api (Part II §9; ambient-vs-composable).
- **replay** → cache-plugin's job, can become an official plugin; **not** a core concern (user, 2026-06-30).
