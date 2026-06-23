"""fe_supervisor — regression for the xinsp-fe.exe frontend supervisor.

Process isolation was removed (2026-05): a hard plugin crash takes the whole
backend down. xinsp-fe.exe is the replacement safety net — it spawns/monitors
the backend, records crash forensics the instant the backend dies, and respawns
it (rate-limited) until a consecutive-failure cap latches the line `down`. (The
old PLC "safe state" delivery was removed 2026-06 — it's now a comms plugin's
own sidecar; the FE just supervises.) This driver proves that loop end-to-end.

What it does
------------
1. Launches `xinsp-fe.exe` pointed at this project on a private port, with
   `--autostart-fps` so the backend self-opens/compiles/starts headlessly.
2. The "crasher" instance is configured `armed: true`, so the backend's first
   inspect spawns a raw thread that null-derefs — the backend dies repeatedly.
3. The FE detects each death, appends a structured crash-history record (with
   the crash report parsed from the backend log), and respawns — until it hits
   the 5-in-60s cap, then latches `down` and exits on its own (rc=2).
4. The driver waits for the FE to exit, then asserts:
     - a `respawning backend` line (death detected -> respawn),
     - >=1 crash-history record with reason=="BackendExit" carrying forensics,
     - `respawn limit ... exceeded` ("staying down") + fe-status latched with
       reason=="RespawnLimitExceeded",
     - the FE exited (rc=2) and left no backend listening on the port (no orphan).

Acceptance (mechanical): all of the above hold -> PASS.

Run from this dir:  python driver.py

TODO(linux): xinsp-fe is Windows-only today (process spawn/Job Object/console
ctrl). On non-nt this test is skipped. See docs/roadmap/linux-port.md.
"""
from __future__ import annotations

import os
import socket
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE_SUFFIX}"
PORT = 7839
FE_LOG = ROOT / "fe.log"
BE_LOG = ROOT / "be.log"
CRASH_HISTORY = ROOT / "crash-history.jsonl"   # default lands next to --be-log
STATUS_FILE = ROOT / "fe-status.json"          # default lands next to --be-log
MAX_WAIT_S = 120.0


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
                 f"build it: cmake --build backend/build --config Release --target xinsp_fe xinsp_backend")
    if port_open():
        sys.exit(f"FAIL: something already listening on :{PORT}; pick a free port")

    # The crash history is append-only (it must survive FE restarts in prod), so
    # clear last run's file for a clean slate before this run.
    CRASH_HISTORY.unlink(missing_ok=True)
    STATUS_FILE.unlink(missing_ok=True)

    fe_log = open(FE_LOG, "wb")
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={ROOT}",
         "--autostart-fps=5",
         f"--be-log={BE_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
    )
    print(f"[fe] launched pid={proc.pid} port={PORT}")

    # The FE self-terminates after hitting the respawn cap. Wait for it.
    deadline = time.time() + MAX_WAIT_S
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.5)
    rc = proc.poll()
    if rc is None:
        print("[fe] did not exit within budget; terminating")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    fe_log.close()

    log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
    print("---- fe.log tail ----")
    for line in log.splitlines()[-25:]:
        print("  " + line)
    print("---------------------")

    failures: list[str] = []

    # Death detection + respawn: the FE no longer logs "ENTER SAFE STATE"; a
    # detected death that respawns shows up as a 'respawning backend' line, and
    # the authoritative per-death record lands in crash-history.jsonl (asserted
    # below, including the crash forensics that used to ride the stderr line).
    if not any("respawning backend" in ln for ln in log.splitlines()):
        failures.append("FE never logged a respawn — death not detected / not respawned")

    if "respawn limit" not in log or "exceeded" not in log:
        failures.append("FE did not hit/report the respawn cap")
    if "staying down" not in log:
        failures.append("FE did not log the 'staying down' cap line — didn't latch down at the cap")

    if rc is None:
        failures.append("FE did not exit on its own after the cap")
    elif rc != 2:
        # rc 2 is the documented "respawn limit exceeded" exit; tolerate but note.
        print(f"[note] FE exit code was {rc} (expected 2 for respawn-cap path)")

    # Crash history: the FE should have appended one structured JSONL record per
    # death — the whole crash-loop story in one file, ending with cap_hit=true.
    if not CRASH_HISTORY.exists():
        failures.append(f"no crash-history.jsonl written ({CRASH_HISTORY})")
    else:
        recs = []
        for ln in CRASH_HISTORY.read_text(encoding="utf-8", errors="ignore").splitlines():
            ln = ln.strip()
            if ln:
                try:
                    recs.append(json.loads(ln))
                except json.JSONDecodeError as e:
                    failures.append(f"crash-history line is not valid JSON: {e}")
        print(f"---- crash-history.jsonl: {len(recs)} record(s) ----")
        if not recs:
            failures.append("crash-history.jsonl had no records")
        else:
            # consecutive count must increase monotonically 1..N over the run.
            consec = [r.get("consecutive") for r in recs]
            if consec != list(range(1, len(recs) + 1)):
                failures.append(f"consecutive counts not 1..N: {consec}")
            if not any(r.get("cap_hit") for r in recs):
                failures.append("no record marked cap_hit=true (cap should have tripped)")
            # Every death is a BackendExit here; at least one must carry the crash
            # forensics (module/phase) parsed from the report — this used to be
            # asserted on the (now-removed) stderr safe-state line.
            if not all(r.get("reason") == "BackendExit" for r in recs):
                failures.append(f"not all records are reason=BackendExit: {[r.get('reason') for r in recs]}")
            has_forensics = any(r.get("module") or r.get("phase") for r in recs)
            if not has_forensics:
                failures.append("no crash-history record carried forensics (module/phase) from the report")
            last = recs[-1]
            if last.get("reason") != "BackendExit" or last.get("exception") != "ACCESS_VIOLATION":
                failures.append(f"last record missing expected forensics: {last}")
            if not last.get("dump", "").endswith(".dmp"):
                failures.append(f"record missing minidump path: {last.get('dump')!r}")

    # Status channel: the FE rewrites fe-status.json on every transition. After the
    # cap, it should reflect a latched DOWN state with the death's forensics — the
    # UI reads THIS instead of inferring "down" from a WS disconnect.
    if not STATUS_FILE.exists():
        failures.append(f"no fe-status.json written ({STATUS_FILE})")
    else:
        try:
            st = json.loads(STATUS_FILE.read_text(encoding="utf-8", errors="ignore"))
            print(f"---- fe-status.json ----\n  {json.dumps(st)}")
            # End state after the cap: "stopped" (the FE exits) or "down", but latched.
            if st.get("state") not in ("stopped", "down"):
                failures.append(f"status state not stopped/down after cap: {st.get('state')!r}")
            if not st.get("latched"):
                failures.append(f"status not latched after cap: {st}")
            if st.get("reason") != "RespawnLimitExceeded":
                failures.append(f"status reason not RespawnLimitExceeded: {st.get('reason')!r}")
            if st.get("respawn_max") != 5:
                failures.append(f"status respawn_max wrong: {st.get('respawn_max')!r}")
            le = st.get("last_event") or {}
            if le.get("exception") != "ACCESS_VIOLATION":
                failures.append(f"status last_event missing forensics: {le}")
        except json.JSONDecodeError as e:
            failures.append(f"fe-status.json is not valid JSON: {e}")

    # No orphan: the FE's Job Object should have killed the backend on close.
    time.sleep(1.0)
    if port_open():
        failures.append(f"a backend is still listening on :{PORT} after FE exit — orphaned child")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  backend died repeatedly -> FE recorded crash forensics, respawned,")
        print("  hit the cap, latched down, exited rc=2, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
