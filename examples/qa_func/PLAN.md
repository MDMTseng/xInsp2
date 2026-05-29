# qa_func — FUNCTIONAL / LIFECYCLE test plan

Viewpoint: the *positive* wiring of the FE/BE split lifecycle — does the right
project/script get opened, compiled, started; does a degraded config (no
script) fail safe by staying up; can a client still attach; and is the FE
config surface (fe.json / CLI / --help) correct. Maps to
`docs/design/fe-be-split-test-plan.md` §2 (AS-I*) and §3 FE-E9/FE-E10.

Complements (does NOT duplicate): `examples/fe_supervisor` (crash storm,
FE-E1), `examples/fe_supervisor_healthy` (FE-E2 happy / FE-E3 orphan),
`examples/plugin_crash_forensics` (BE minidump). Safety/respawn/orphan paths
are owned there; this driver owns the autostart + config functional surface.

All cases spawn their own exe on a private port in **7850-7869**, name-guarded
kill, SKIP on non-nt.

| ID | Case | Pass criteria |
|---|---|---|
| AS-I1 | autostart happy path (`--project --script`) | be.log shows `autostart: open_project` then `compile_and_load`; WS port accepts; BE does not exit |
| AS-I2 | `--autostart-fps=10` starts continuous mode | log shows `autostart: start 10 fps`; a passive late client sees ≥3 unsolicited `vars` frames in 2 s (timer pump live) |
| AS-I3 | `--script` override | starting with `--script=alt_inspect.cpp` compiles THAT file, not project.json's `inspect.cpp`; the `script` VAR == 2 (default stamps 1) |
| AS-I4 | `--project` with no script (open-only) | log shows the `… open only` notice, no `compile_and_load`; **BE stays up**; late client `ping` succeeds |
| AS-I7 | late client attach | client connecting after autostart can `version` + `ping` + `run` (count VAR returned) — port not monopolised |
| FE-E9 | fe.json honoured + CLI override | A: FE with no flags reads `fe.json` (project/script/autostart_fps=8) → BE log shows open_project+compile_and_load+`start 8 fps`. B: same fe.json + CLI `--autostart-fps=0` → no `autostart: start` line (CLI wins) |
| FE-E10 | `xinsp-fe --help` | exit 0; prints `Usage: xinsp-fe`; lists `--project`/`--autostart-fps`/`--config`; spawns no backend |

Ports: AS-I1 7850, AS-I2 7851, AS-I3 7852, AS-I4 7853, AS-I7 7854,
FE-E9/A 7855, FE-E9/B 7856. FE-E10 spawns nothing.

Fixtures:
- `project.json` + `inspect.cpp` (stamps `script=1`) + `instances/counter`
  (raw_thread_crash, armed:false → healthy) + `plugins/raw_thread_crash`
  (copied from fe_supervisor_healthy).
- `alt_inspect.cpp` — identical but stamps `script=2` (the override target).
- `proj_noscript/project.json` — no `script`, no instances (open-only case).
- `_fe_cfg/` — scratch cwd the FE-E9 driver writes `fe.json` into at runtime.

Run: `python driver.py` (all) or `python driver.py AS-I2` (one).
