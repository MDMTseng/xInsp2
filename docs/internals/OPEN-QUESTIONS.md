# Open Questions — deferred decisions (autonomous execution log)

**Context:** User stepped away 2026-07-01 mid-roadmap (Parts I–IV of `core_fix_plan.md`).
Standing instruction: log deferrable decisions here, proceed on the recorded default, only
stop and ask if genuinely blocked. Each item records **the default I am proceeding with**,
**why it is safe to defer**, and **what would flip it**. Resolve with the user on return.

---

## OQ-1 — Race detection on Windows (MSVC has no ThreadSanitizer)
- **Default:** ASan via MSVC `/fsanitize=address` for memory safety; for *races*, strengthen the
  probabilistic stress probes (`race_probe`, `test_set_def_race`, `test_image_pool_stress`) into a
  higher-iteration suite, and attempt a `clang-cl -fsanitize=thread` config — accept it only if it
  genuinely builds+runs on Windows.
- **Why deferrable:** real TSan most likely needs a **Linux CI lane** (repo already has a
  `docs/roadmap/linux-port.md` direction + TODO(linux) markers). That is a larger infra decision.
- **Flips if:** user wants a Linux CI lane stood up now, or accepts a clang-only test-suite build.
- **Impacts:** Tier 0 T0.1; Tier 1 T1.3 (TSan stress on Part III G2).

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

---

### Resolved (recorded for trail)
- **II-vs-III ordering** → **parallel tracks** after Tier 0 (user: "能平行就平行", 2026-07-01).
- **Gated pipeline vs single auto-workflow** → **gated, phase-by-phase** with build+test gates (user-approved).
- **compress / log / status pluginization** → **NO**, stay ambient host_api (Part II §9; ambient-vs-composable).
- **replay** → cache-plugin's job, can become an official plugin; **not** a core concern (user, 2026-06-30).
