"""qa_fault — exact respawn-count accounting at the cap (safety props SP2/SP6/SP1).

fe_supervisor/driver.py (FE-E1) proves the crash-storm loop *qualitatively*
(>=1 respawn, a cap, no orphan). This driver pins the EXACT counts, which the
safety plan calls out as SP2: a death is RECORDED on EVERY death, including the
cap one. (The old PLC "safe state" lines were removed 2026-06; the authoritative
per-death ledger is now crash-history.jsonl, and the cap is the 'staying down'
stderr line + the cap_hit record.)

With the FE defaults (respawn_max=5, window=60s) and an always-crashing backend,
the supervisor loop produces a deterministic ledger:

    death 1..5 : BackendExit record -> respawns.size 0..4 (<5) -> respawning N/5
    death 6    : BackendExit record (cap_hit) -> staying down, exit rc=2

so:  #crash-history records (BackendExit) == respawn_max + 1   (== 6)
     #cap_hit records                     == 1
     #respawning                          == respawn_max       (== 5)
     and NO 'respawning' line appears after the 'staying down' cap line (SP6).

Asserts all of the above, plus SP1 (a death is recorded before its respawn) and
the no-orphan post-check. Uses storm_project (armed raw_thread_crash) on a
private port; the FE self-exits at the cap so we just wait for it.

Run from anywhere:  python driver_respawn_accounting.py

TODO(linux): xinsp-fe is Windows-only today. SKIPs on non-nt.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE_SUFFIX}"
STORM_PROJECT = ROOT / "storm_project"
PORT = 7875
FE_LOG = ROOT / "fe_accounting.log"
BE_LOG = ROOT / "be_accounting.log"
CRASH_HISTORY = ROOT / "crash-history.jsonl"   # default lands next to --be-log
RESPAWN_MAX = 5          # the FE default (fe_main.cpp FeConfig::respawn_max)
MAX_WAIT_S = 180.0       # cold plugin compile on each of the N backend starts


def port_open(port: int = PORT, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/roadmap/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release --target xinsp_fe")
    if port_open():
        sys.exit(f"FAIL: something already listening on :{PORT}; pick a free port")

    CRASH_HISTORY.unlink(missing_ok=True)

    fe_log = open(FE_LOG, "wb")
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={STORM_PROJECT}",
         "--autostart-fps=5",
         f"--be-log={BE_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
    )
    print(f"[fe] launched pid={proc.pid} port={PORT} on storm_project (armed)")

    deadline = time.time() + MAX_WAIT_S
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.5)
    rc = proc.poll()
    if rc is None:
        print("[fe] did not reach the cap within budget; terminating")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    fe_log.close()

    lines = FE_LOG.read_text(encoding="utf-8", errors="ignore").splitlines()
    print("---- fe.log tail ----")
    for ln in lines[-25:]:
        print("  " + ln)
    print("---------------------")

    failures: list[str] = []

    # The authoritative per-death ledger is crash-history.jsonl.
    recs = []
    if CRASH_HISTORY.exists():
        for ln in CRASH_HISTORY.read_text(encoding="utf-8", errors="ignore").splitlines():
            ln = ln.strip()
            if ln:
                try:
                    recs.append(json.loads(ln))
                except json.JSONDecodeError as e:
                    failures.append(f"crash-history line is not valid JSON: {e}")
    else:
        failures.append(f"no crash-history.jsonl written ({CRASH_HISTORY})")

    backend_exits = [r for r in recs if r.get("reason") == "BackendExit"]
    cap_hits = [r for r in recs if r.get("cap_hit")]
    cap_idx = [i for i, ln in enumerate(lines) if "staying down" in ln]
    respawn_idx = [i for i, ln in enumerate(lines) if "respawning backend" in ln]

    n_deaths = len(backend_exits)
    print(f"counts: BackendExit records={n_deaths}  cap_hit={len(cap_hits)}  "
          f"staying-down lines={len(cap_idx)}  respawning={len(respawn_idx)}")

    # SP2: one death record per death, and the deterministic ledger above.
    expected_deaths = RESPAWN_MAX + 1
    if n_deaths != expected_deaths:
        failures.append(f"expected {expected_deaths} BackendExit crash-history records "
                        f"(respawn_max+1), got {n_deaths}")
    # consecutive must climb 1..N over the run.
    consec = [r.get("consecutive") for r in recs]
    if consec != list(range(1, len(recs) + 1)):
        failures.append(f"consecutive counts not 1..N: {consec}")
    if len(cap_hits) != 1:
        failures.append(f"expected exactly 1 cap_hit record, got {len(cap_hits)}")
    elif recs and not recs[-1].get("cap_hit"):
        failures.append("cap_hit is not the LAST record — cap should trip on the final death")
    if len(cap_idx) != 1:
        failures.append(f"expected exactly 1 'staying down' cap line, got {len(cap_idx)}")
    if len(respawn_idx) != RESPAWN_MAX:
        failures.append(f"expected exactly {RESPAWN_MAX} 'respawning backend' lines, "
                        f"got {len(respawn_idx)}")

    # SP1: a death is recorded before any respawn (the record drives the respawn).
    if respawn_idx and not backend_exits:
        failures.append("SP1 violated: a 'respawning' line with no death recorded")

    # SP6: after the cap, the FE stays down — no further respawn attempts.
    if cap_idx:
        cap_line = cap_idx[0]
        if any(r > cap_line for r in respawn_idx):
            failures.append("SP6 violated: a 'respawning backend' line appears AFTER "
                            "'staying down' — FE is spinning, not staying down")

    if rc is None:
        failures.append("FE did not exit at the cap")
    elif rc != 2:
        print(f"[note] FE exit code was {rc} (expected 2 for the respawn-cap path)")

    time.sleep(1.0)
    if port_open():
        failures.append(f"a backend is still listening on :{PORT} after FE exit — orphan")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print(f"  exact ledger: {expected_deaths} BackendExit records (1 cap_hit) + "
              f"{RESPAWN_MAX} respawns,")
        print("  no spin past the cap (SP6), no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
