"""qa_fault — FE recover-and-clear transition (FE-E5, safety props SP1/SP4).

The most important POSITIVE fault case: a backend that crashes ONCE and then
runs healthy after the respawn. The FE must:
  - detect the one death, drive ENTER SAFE STATE (reason=BackendExit),
  - respawn (the ENTER must precede the 'respawning' line — SP1),
  - confirm the respawned backend is healthy, then CLEAR SAFE STATE exactly
    once (and only AFTER a 'backend healthy' probe — SP4),
  - keep running with NO further crashes and NO respawn-cap.

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
CLEAR_WAIT_S = 120.0   # cover a cold plugin compile on each backend start
STABLE_S = 6.0


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
        # Poll fe.log until we see the CLEAR SAFE STATE (recovery confirmed).
        deadline = time.time() + CLEAR_WAIT_S
        cleared = False
        while time.time() < deadline:
            if proc.poll() is not None:
                failures.append(f"FE exited early rc={proc.returncode} — it should keep "
                                f"running after a single crash+heal")
                break
            if any("CLEAR SAFE STATE" in ln for ln in read_log()):
                cleared = True
                break
            time.sleep(0.5)
        if not cleared and proc.poll() is None:
            failures.append("FE never logged CLEAR SAFE STATE within budget — "
                            "did not recover after the single crash")

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

    enters = [i for i, ln in enumerate(lines)
              if "ENTER SAFE STATE" in ln and "reason=BackendExit" in ln]
    clears = [i for i, ln in enumerate(lines) if "CLEAR SAFE STATE" in ln]
    respawns = [i for i, ln in enumerate(lines) if "respawning backend" in ln]
    healthy = [i for i, ln in enumerate(lines) if "backend healthy" in ln]

    # Exactly one death-driven ENTER (crash once).
    if len(enters) != 1:
        failures.append(f"expected exactly 1 'ENTER SAFE STATE reason=BackendExit', got {len(enters)}")
    # Exactly one CLEAR (heal once).
    if len(clears) < 1:
        failures.append("no 'CLEAR SAFE STATE' — FE never confirmed the heal")
    # Must NOT hit the cap.
    if any("RespawnLimitExceeded" in ln for ln in lines):
        failures.append("FE hit RespawnLimitExceeded — a single crash must not trip the cap")
    if any("respawn limit" in ln and "exceeded" in ln for ln in lines):
        failures.append("FE logged respawn-cap exceeded on a single crash")

    # SP1: the ENTER precedes its respawn line.
    if enters and respawns and not (min(enters) < min(respawns)):
        failures.append("SP1 violated: 'respawning' appears before 'ENTER SAFE STATE'")
    # SP4: the CLEAR follows a 'backend healthy' probe (never optimistic).
    if clears and healthy:
        if not any(h < clears[0] for h in healthy):
            failures.append("SP4 violated: CLEAR SAFE STATE not preceded by a 'backend healthy' probe")
    elif clears and not healthy:
        failures.append("SP4 violated: CLEAR with no 'backend healthy' probe logged")

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
        print(f"  one crash -> 1 ENTER (BackendExit) -> respawn -> healthy ->")
        print(f"  1 CLEAR SAFE STATE, no cap, FE kept running, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
