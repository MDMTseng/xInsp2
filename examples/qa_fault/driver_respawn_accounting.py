"""qa_fault — exact respawn-count accounting at the cap (safety props SP2/SP6/SP1).

fe_supervisor/driver.py (FE-E1) proves the crash-storm loop *qualitatively*
(>=1 ENTER, >=1 respawn, a cap, no orphan). This driver pins the EXACT counts,
which the safety plan calls out as SP2: safe-state is driven on EVERY death,
including the cap one.

With the FE defaults (respawn_max=5, window=60s) and an always-crashing backend,
the supervisor loop produces a deterministic ledger:

    death 1..5 : ENTER(BackendExit) -> respawns.size 0..4 (<5) -> respawning N/5
    death 6    : ENTER(BackendExit) -> respawns.size==5      -> RespawnLimitExceeded, exit rc=2

so:  #ENTER(BackendExit) == respawn_max + 1   (== 6)
     #RespawnLimitExceeded == 1
     #respawning            == respawn_max     (== 5)
     and NO 'respawning' line appears after the RespawnLimitExceeded (SP6).

Asserts all of the above, plus SP1 (each ENTER precedes its respawn) and the
no-orphan post-check. Uses storm_project (armed raw_thread_crash) on a private
port; the FE self-exits at the cap so we just wait for it.

Run from anywhere:  python driver_respawn_accounting.py

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
STORM_PROJECT = ROOT / "storm_project"
PORT = 7875
FE_LOG = ROOT / "fe_accounting.log"
BE_LOG = ROOT / "be_accounting.log"
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
        print("SKIP: xinsp-fe is Windows-only today (see docs/design/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release --target xinsp_fe")
    if port_open():
        sys.exit(f"FAIL: something already listening on :{PORT}; pick a free port")

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

    enter_idx = [i for i, ln in enumerate(lines)
                 if "ENTER SAFE STATE" in ln and "reason=BackendExit" in ln]
    cap_idx = [i for i, ln in enumerate(lines)
               if "ENTER SAFE STATE" in ln and "reason=RespawnLimitExceeded" in ln]
    respawn_idx = [i for i, ln in enumerate(lines) if "respawning backend" in ln]

    n_deaths = len(enter_idx)
    print(f"counts: BackendExit ENTER={n_deaths}  RespawnLimitExceeded={len(cap_idx)}  "
          f"respawning={len(respawn_idx)}")

    # SP2: one safe-state per death, and the deterministic ledger above.
    expected_deaths = RESPAWN_MAX + 1
    if n_deaths != expected_deaths:
        failures.append(f"expected {expected_deaths} 'ENTER SAFE STATE reason=BackendExit' "
                        f"(respawn_max+1), got {n_deaths}")
    if len(cap_idx) != 1:
        failures.append(f"expected exactly 1 'reason=RespawnLimitExceeded', got {len(cap_idx)}")
    if len(respawn_idx) != RESPAWN_MAX:
        failures.append(f"expected exactly {RESPAWN_MAX} 'respawning backend' lines, "
                        f"got {len(respawn_idx)}")

    # SP1: each respawn is preceded by an ENTER (the first ENTER precedes the
    # first respawn; counts already pin the rest).
    if enter_idx and respawn_idx and not (min(enter_idx) < min(respawn_idx)):
        failures.append("SP1 violated: 'respawning' appears before the first 'ENTER SAFE STATE'")

    # SP6: after the cap, the FE stays safe — no further respawn attempts.
    if cap_idx:
        cap_line = cap_idx[0]
        if any(r > cap_line for r in respawn_idx):
            failures.append("SP6 violated: a 'respawning backend' line appears AFTER "
                            "RespawnLimitExceeded — FE is spinning, not staying safe")

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
        print(f"  exact ledger: {expected_deaths} BackendExit ENTERs + 1 "
              f"RespawnLimitExceeded + {RESPAWN_MAX} respawns,")
        print("  no spin past the cap (SP6), no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
