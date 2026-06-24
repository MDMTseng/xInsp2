"""qa_fault — FE recover-and-clear transition (FE-E5, safety props SP1/SP4).

The most important POSITIVE fault case: a backend that crashes ONCE and then
runs healthy after the respawn. The FE must:
  - detect the one death and record it (crash-history reason=BackendExit),
  - respawn (the death record must precede the 'respawning' line — SP1),
  - confirm the respawned backend is healthy ('backend healthy' AFTER the
    'respawning' line — the recover transition, SP4),
  - keep running with NO further crashes and NO respawn-cap.

(The old PLC "safe state" lines were removed 2026-06; the FE now just supervises.
The recover transition is 'backend healthy' after a 'respawning backend' line,
backed by a single BackendExit crash-history record with no cap_hit.)

The fixture (heal_project/crash_once_heal) crashes on the first inspect of a
fresh instance, drops a marker file, then heals on the next backend start. The
marker survives the process restart; we delete it up front so the run is
repeatable.

Because a healthy FE never self-exits, this driver POLLS fe.log until it sees
the CLEAR (or times out), confirms stability for a few seconds, then stops the
FE and checks no orphan remains.

Run from anywhere:  python driver_fe_recover.py

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
PORT = 7874
HEAL_PROJECT = ROOT / "heal_project"
FE_LOG = ROOT / "fe_recover.log"
BE_LOG = ROOT / "be_recover.log"
CRASH_HISTORY = ROOT / "crash-history.jsonl"   # default lands next to --be-log
STATUS_FILE = ROOT / "fe-status.json"          # default lands next to --be-log
CLEAR_WAIT_S = 120.0   # cover a cold plugin compile on each backend start
STABLE_S = 6.0


def recovered_in_log(lines: list[str]) -> bool:
    """True once a 'backend healthy' line appears AFTER the LAST 'respawning
    backend' line — the final respawned instance came back up (the recover
    transition that used to be logged as CLEAR SAFE STATE)."""
    respawns = [i for i, ln in enumerate(lines) if "respawning backend" in ln]
    if not respawns:
        return False
    return any("backend healthy" in ln for ln in lines[respawns[-1] + 1:])


def port_open(port: int = PORT, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def clear_markers() -> None:
    for m in HEAL_PROJECT.glob("instances/*/.crashed_once.marker"):
        try:
            m.unlink()
        except OSError:
            pass


def read_log() -> list[str]:
    try:
        return FE_LOG.read_text(encoding="utf-8", errors="ignore").splitlines()
    except OSError:
        return []


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/roadmap/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release --target xinsp_fe")
    if port_open():
        sys.exit(f"FAIL: something already listening on :{PORT}; pick a free port")

    clear_markers()
    CRASH_HISTORY.unlink(missing_ok=True)
    STATUS_FILE.unlink(missing_ok=True)

    fe_log = open(FE_LOG, "wb")
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={HEAL_PROJECT}",
         "--autostart-fps=5",
         f"--be-log={BE_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
    )
    print(f"[fe] launched pid={proc.pid} port={PORT} on heal_project")

    failures: list[str] = []
    try:
        # Poll fe.log until we see the recover transition ('backend healthy'
        # after a 'respawning backend' line) — recovery confirmed.
        deadline = time.time() + CLEAR_WAIT_S
        cleared = False
        while time.time() < deadline:
            if proc.poll() is not None:
                failures.append(f"FE exited early rc={proc.returncode} — it should keep "
                                f"running after a single crash+heal")
                break
            if recovered_in_log(read_log()):
                cleared = True
                break
            time.sleep(0.5)
        if not cleared and proc.poll() is None:
            failures.append("FE never recovered within budget ('backend healthy' after "
                            "'respawning backend') — did not heal after the single crash")

        # Stability window: no further crash / respawn after the heal.
        if cleared:
            print("[fe] CLEAR seen; checking stability...")
            time.sleep(STABLE_S)
            if proc.poll() is not None:
                failures.append(f"FE exited rc={proc.returncode} during the stability window")
    finally:
        fe_log.flush()
        # Stop the (healthy, still-running) FE. Job Object reaps the BE.
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
        fe_log.close()

    lines = read_log()
    print("---- fe.log tail ----")
    for ln in lines[-25:]:
        print("  " + ln)
    print("---------------------")

    respawns = [i for i, ln in enumerate(lines) if "respawning backend" in ln]
    healthy = [i for i, ln in enumerate(lines) if "backend healthy" in ln]

    # crash-history.jsonl: exactly one BackendExit death record (crash once), no cap_hit.
    recs = []
    if CRASH_HISTORY.exists():
        for ln in CRASH_HISTORY.read_text(encoding="utf-8", errors="ignore").splitlines():
            ln = ln.strip()
            if ln:
                try:
                    recs.append(json.loads(ln))
                except json.JSONDecodeError as e:
                    failures.append(f"crash-history line is not valid JSON: {e}")
    backend_exits = [r for r in recs if r.get("reason") == "BackendExit"]
    if len(backend_exits) != 1:
        failures.append(f"expected exactly 1 BackendExit crash-history record, got {len(backend_exits)} "
                        f"(reasons: {[r.get('reason') for r in recs]})")
    if any(r.get("cap_hit") for r in recs):
        failures.append("a crash-history record is cap_hit=true — a single crash must not trip the cap")
    # Exactly one respawn (heal after the single crash).
    if len(respawns) != 1:
        failures.append(f"expected exactly 1 'respawning backend' line, got {len(respawns)}")
    # The recover transition must have happened.
    if not recovered_in_log(lines):
        failures.append("no 'backend healthy' after 'respawning backend' — FE never confirmed the heal")
    # Must NOT hit the cap.
    if any("staying down" in ln or "RespawnLimitExceeded" in ln for ln in lines):
        failures.append("FE latched down — a single crash must not trip the cap")
    if any("respawn limit" in ln and "exceeded" in ln for ln in lines):
        failures.append("FE logged respawn-cap exceeded on a single crash")

    # SP1: a death is recorded before its respawn (the record drives the respawn).
    if respawns and not backend_exits:
        failures.append("SP1 violated: a 'respawning' line with no death recorded")
    # SP4: the recover ('backend healthy') follows the respawn, never optimistic —
    # this is exactly what recovered_in_log() asserts above.

    # No orphan after we stopped the FE.
    time.sleep(1.0)
    if port_open():
        failures.append(f"a backend is still listening on :{PORT} after FE stop — orphan")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print(f"  one crash -> 1 BackendExit record -> respawn -> backend healthy")
        print(f"  (recovered), no cap, FE kept running, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
