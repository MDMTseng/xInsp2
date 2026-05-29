# qa_fault — fault-injection test plan (QA viewpoint: FAULT)

Reliability / fault-injection coverage for the FE/BE split
([`../../docs/design/fe-be-split.md`](../../docs/design/fe-be-split.md),
[`fe-be-split-test-plan.md`](../../docs/design/fe-be-split-test-plan.md)).

This viewpoint owns the **negative / degrade-safely** cases the central plan
flags as gaps: §2 AS-I4/5/6 (backend autostart negatives), §3 FE-E4/E5/E6 (FE
supervisor edge transitions), and the §5 safety properties (SP1/SP2/SP4/SP6).
It deliberately does **not** re-cover the crash-storm happy/cap loop already
proven by `examples/fe_supervisor/driver.py` (FE-E1) — except to assert the one
property that example only checks loosely: **exact respawn-count accounting**.

All drivers are Windows-only (the FE + autostart spawn path is `#ifdef _WIN32`);
each SKIPs with rc=0 on non-`nt`. See `docs/design/linux-port.md`.

Private ports: **7870–7889** (no overlap with other examples).

## Fixtures

| Fixture | What it is |
|---|---|
| `heal_project/` | project whose `crash_once_heal` plugin crashes the BE on the FIRST `process()` of a fresh on-disk instance, drops a marker in its instance folder, then runs healthy on every later BE start. Drives FE-E5 (crash once → heal) deterministically across a respawn. |
| `badscript_project/` | project whose `inspect.cpp` has a hard syntax error — `compile_and_load` fails, BE must stay up. Drives AS-I6. |
| `storm_project/` | a copy of `raw_thread_crash` armed `true` (like fe_supervisor) — used only for QF-I7 cap-accounting, on a private port. |

`heal_project` and `storm_project` ship plugin **source only** (`src/plugin.cpp`
+ `plugin.json`); `open_project` compiles each into `build/<name>.dll` on every
BE start, so the fixtures are self-contained and need no pre-built DLL.

## Cases

| ID | Maps to | Fault injected | Pass criteria |
|---|---|---|---|
| **QF-I1** | AS-I5 | BE `--project=<nonexistent dir>` | log carries an `open_project` failure (no `project opened`); **BE process stays alive**; WS port still accepts; a late client `version`/`ping` works |
| **QF-I2** | AS-I6 | BE `--project=badscript_project` (script won't compile) | log shows `compile` failure / `compile_finished` error; **BE stays alive**; port accepts; client can still `open_project` a good project and `ping` |
| **QF-I3** | AS-I4 (bonus) | BE `--project` whose project.json has no script and no `--script` | log shows `no script … open only`; BE stays alive; port accepts |
| **QF-E6** | FE-E6 | FE `--backend=<bad path>` | FE `CreateProcess` fails → `ENTER SAFE STATE reason=BackendExit` → **FE exits nonzero (rc=1)**; no BE ever on the port |
| **QF-E5** | FE-E5, SP1/SP4 | `heal_project`: BE crashes once, heals after respawn | exactly **one** `ENTER SAFE STATE reason=BackendExit`; then a `CLEAR SAFE STATE`; `ENTER` precedes its `respawning` line (SP1); the `CLEAR` follows a healthy probe (SP4); **no** `RespawnLimitExceeded`; FE keeps running until the driver Ctrl-C's it; clean exit; no orphan |
| **QF-I7** | SP2 | `storm_project` armed, tiny respawn window via `--be-log` parse | count of `ENTER SAFE STATE reason=BackendExit` == number of BE deaths == `respawn_max` ; **exactly one** `ENTER SAFE STATE reason=RespawnLimitExceeded`; the cap-`ENTER` is the last safe-state line (SP6 — no further `respawning`) |

## Safety properties asserted (in addition to functional pass)

- **SP1** (QF-E5, QF-I7): every `ENTER SAFE STATE` precedes its matching
  `respawning backend` line in `fe.log`.
- **SP2** (QF-I7): `#BackendExit ENTER` == `#deaths`, plus exactly one
  `RespawnLimitExceeded`.
- **SP4** (QF-E5): a `CLEAR SAFE STATE` only appears after a `backend healthy`
  line (the confirmed-healthy probe), never optimistically.
- **SP6** (QF-I7): after `RespawnLimitExceeded`, `fe.log` has no further
  `respawning backend` line — the FE stays safe, doesn't spin.

## Not covered here (owned elsewhere / Phase 2)

- FE-E4 `PortUnresponsive` (hang plugin under a long `--watchdog`) — needs a
  wedged-accept-loop fixture; deferred.
- FE-E3 orphan-kill on hard `taskkill` of the FE — covered by Round-2 work
  (test plan FE-E3); QF drivers only do the post-exit port check (SP5-lite).
- Crash forensics content (minidump/`threads[]`) — owned by
  `examples/plugin_crash_forensics`.

## Run

```
python examples/qa_fault/driver_autostart_negatives.py   # QF-I1, QF-I2, QF-I3
python examples/qa_fault/driver_fe_badexe.py             # QF-E6
python examples/qa_fault/driver_fe_recover.py            # QF-E5
python examples/qa_fault/driver_respawn_accounting.py    # QF-I7
```

C++ unit (respawn-window arithmetic, pure logic): `backend/tests/test_qa_fault.cpp`.
