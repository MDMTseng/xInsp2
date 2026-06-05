# QA team loop — Round 5 (countdown 6 → 5) · fill coverage: gaps #1 & #4

After the dryness review the user asked to fill coverage. Closed the two
highest-value gaps from the coverage assessment.

## Gap #1 — a compile-broken line must NOT look healthy (product + test)
**Was:** the BE printed `autostart: ready` even when the script failed to
compile, so the FE (and an operator) saw a bound port + "ready" and called a
line healthy that could never inspect — a blind line running silently.

**Fix (product):**
- BE: after autostart `compile_and_load`, check `g_script.ok()`. If a script was
  expected but didn't load → log `autostart: degraded …` and **withhold**
  `autostart: ready` (port stays up so an operator can recompile).
- FE: the boot gate now treats `autostart: degraded` as a failed boot →
  `enter_safe_state(BootTimeout)` → respawn (deterministic compile error churns
  to the cap and stays safe). Never reports such a line healthy.

**Test:** `examples/qa_fault/driver_fe_degraded.py` (reuses the won't-compile
`badscript_project`). Asserts: FE never logs `backend healthy`; be.log shows
`autostart: degraded` and **not** `autostart: ready`; FE drives BootTimeout,
respawns, hits the cap; no orphan. **PASS.**

## Gap #4 — test the REAL respawn math, not a copy (test quality)
The cap/window decision was a file-static loop in `fe_main.cpp`; the units
(`test_qa_fault`, `test_qa_race`) had hand-copied it, so a refactor could drift
unseen. Extracted it to `backend/include/xi/xi_respawn_policy.hpp`
(`xi::respawn_should_trip`); `fe_main.cpp` now calls it and both units delegate
to it. The units now guard the production code, not a model.

## Result
- ctest **11/11** (units now exercise the real respawn fn).
- New driver PASS; regression sweep green: `fe_supervisor`,
  `fe_supervisor_healthy`, `qa_func`, `qa_race/driver_boot_hang`,
  `qa_fault/driver_respawn_accounting`.

## Coverage now
Closed: crash loop, boot hang, **compile-broken line**, respawn math (real fn),
sink contract, crash parser, extension decision, orphan guarantee, autostart
matrix + negatives.
Remaining (infra/Phase 2, not quick fixes): serve-time wedge deep heartbeat,
extension-host e2e, soak/leak, Linux. Countdown 5.
