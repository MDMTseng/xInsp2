"""qa_recover — FE supervisor recover-and-clear regression (Phase G, #92).

fe_supervisor proves the FE's *give-up* path: a backend that crashes forever ->
respawn -> hit the cap -> latch down. This proves the *happy exit* the cap path
can't: a backend that crashes a FEW times and then RECOVERS must heal back to a
"healthy" (un-latched) state, and the FE must NOT hit the cap (test plan FE-E5 /
safety property SP4). (The old PLC "safe state" lines were removed 2026-06; the
FE now just supervises — the recover transition shows up as fe-status returning
to state=="healthy" with latched==false and no cap_hit record.)

What it does
------------
1. Picks a fresh marker file and exports XI_QA_RECOVER_MARKER (inherited
   FE -> backend -> plugin). The `crash_then_heal` plugin crashes the backend its
   first `crash_count` (2) starts, counting in that file (which survives the
   process dying), then returns normally forever.
2. Launches `xinsp-fe.exe` on this project with `--autostart-fps`.
3. The backend crashes twice -> FE records each death + respawns. The 3rd backend
   instance is healthy -> FE returns fe-status to state=="healthy".
4. Asserts from the FE log + crash-history.jsonl + fe-status.json:
     - >=1 crash-history record reason=="BackendExit" (death detected + recorded),
     - >=1 `respawning backend` stderr line,
     - a `backend healthy` line AFTER a `respawning backend` line (the recover —
       the whole point) and fe-status state=="healthy", latched==false,
     - NO `staying down` / RespawnLimitExceeded and no cap_hit record (recovered
       before the cap),
     - the marker counted EXACTLY `crash_count` crashes,
     - the backend is up at the end and the FE is still running (didn't give up).
5. Stops the FE and asserts no orphan.

Run from this dir:  python driver.py

TODO(linux): xinsp-fe is Windows-only today. Skips on non-nt.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "qa" / "lib"))
from ports import free_port, backend_exe, fe_exe  # noqa: E402
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = fe_exe()
PORT = free_port()  # ephemeral (was 7861); no squatter cross-talk
CRASH_COUNT = 2                       # must match instances/healer/instance.json
FE_LOG = ROOT / "fe.log"
BE_LOG = ROOT / "be.log"
CRASH_HISTORY = ROOT / "crash-history.jsonl"   # default lands next to --be-log
STATUS_FILE = ROOT / "fe-status.json"          # default lands next to --be-log
MAX_WAIT_S = 120.0


def recovered_in_log(lines: list[str], min_respawns: int = CRASH_COUNT) -> bool:
    """True once all expected crashes have happened (>= min_respawns 'respawning
    backend' lines) AND a 'backend healthy' line appears AFTER the LAST respawn —
    i.e. the final respawned instance came back up and stayed up (the recover
    transition that used to be logged as CLEAR SAFE STATE). Using the LAST respawn
    (not the first) avoids a false positive from a healthy blip BETWEEN crashes."""
    respawns = [i for i, ln in enumerate(lines) if "respawning backend" in ln]
    if len(respawns) < min_respawns:
        return False
    return any("backend healthy" in ln for ln in lines[respawns[-1] + 1:])


def port_open(port: int = PORT, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def read_marker(path: Path) -> int:
    try:
        return int(path.read_text().strip() or "0")
    except (OSError, ValueError):
        return 0


def main() -> int:
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release "
                 f"--target xinsp_fe xinsp_backend")
    if port_open():
        sys.exit(f"FAIL: something already listening on :{PORT}; pick a free port")

    # Clear last run's structured state for a clean slate (crash-history is
    # append-only and must survive FE restarts in prod).
    CRASH_HISTORY.unlink(missing_ok=True)
    STATUS_FILE.unlink(missing_ok=True)

    marker = Path(tempfile.gettempdir()) / "xinsp2_qa_recover.marker"
    marker.unlink(missing_ok=True)
    env = dict(os.environ)
    env["XI_QA_RECOVER_MARKER"] = str(marker)

    fe_log = open(FE_LOG, "wb")
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={ROOT}",
         "--autostart-fps=4",
         "--boot-timeout-ms=180000",
         f"--be-log={BE_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
        env=env,
    )
    print(f"[fe] launched pid={proc.pid} port={PORT} marker={marker}")

    failures: list[str] = []
    recovered = False
    try:
        # Wait until: the backend crashed crash_count times (marker), a respawned
        # instance came back healthy (the recover transition), and the backend is
        # up — or the FE gives up / times out.
        deadline = time.time() + MAX_WAIT_S
        while time.time() < deadline:
            if proc.poll() is not None:
                # The FE only self-exits on the cap path; that would be a failure
                # here (it should have recovered first).
                break
            log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
            if "staying down" in log:
                break  # gave up — assertions below will flag it
            if (read_marker(marker) >= CRASH_COUNT
                    and recovered_in_log(log.splitlines())
                    and port_open()):
                recovered = True
                break
            time.sleep(0.5)

        # Let it sit healthy a moment so a flaky "up then re-crash" would show.
        if recovered:
            time.sleep(3.0)
            recovered = proc.poll() is None and port_open()

        log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
        lines = log.splitlines()

        if not any("respawning backend" in ln for ln in lines):
            failures.append("FE never logged a respawn — crash not detected / not respawned")
        if not recovered_in_log(lines):
            failures.append("no 'backend healthy' after a 'respawning backend' — the "
                            "recover transition never happened (FE-E5)")
        if "staying down" in log or "RespawnLimitExceeded" in log:
            failures.append("FE latched down at the cap — it gave up instead of recovering")

        # crash-history.jsonl: one BackendExit record per death, none cap_hit.
        recs = []
        if CRASH_HISTORY.exists():
            for ln in CRASH_HISTORY.read_text(encoding="utf-8", errors="ignore").splitlines():
                ln = ln.strip()
                if ln:
                    try:
                        recs.append(json.loads(ln))
                    except json.JSONDecodeError as e:
                        failures.append(f"crash-history line is not valid JSON: {e}")
        if len(recs) < 1:
            failures.append("no crash-history record — death not detected / not recorded")
        elif not all(r.get("reason") == "BackendExit" for r in recs):
            failures.append(f"crash-history has non-BackendExit reasons: {[r.get('reason') for r in recs]}")
        if any(r.get("cap_hit") for r in recs):
            failures.append("a crash-history record is cap_hit=true — FE hit the cap (should have recovered)")

        # fe-status.json: after recovery the FE returns to a healthy, un-latched state.
        if STATUS_FILE.exists():
            try:
                st = json.loads(STATUS_FILE.read_text(encoding="utf-8", errors="ignore"))
                print(f"---- fe-status.json ----\n  {json.dumps(st)}")
                if recovered:
                    if st.get("state") != "healthy":
                        failures.append(f"fe-status state not 'healthy' after recovery: {st.get('state')!r}")
                    if st.get("latched"):
                        failures.append(f"fe-status still latched after recovery: {st}")
            except json.JSONDecodeError as e:
                failures.append(f"fe-status.json is not valid JSON: {e}")
        else:
            failures.append(f"no fe-status.json written ({STATUS_FILE})")

        crashes = read_marker(marker)
        if crashes != CRASH_COUNT:
            failures.append(f"expected exactly {CRASH_COUNT} crashes, marker says {crashes}")

        if proc.poll() is not None:
            failures.append(f"FE exited (rc={proc.poll()}) instead of staying up after recovery")
        elif not port_open():
            failures.append("backend not listening after recovery — not actually healed")
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
        fe_log.close()
        marker.unlink(missing_ok=True)

    time.sleep(1.0)
    if port_open():
        failures.append(f"backend still listening on :{PORT} after FE exit — orphan")

    print("---- fe.log tail ----")
    for line in FE_LOG.read_text(encoding="utf-8", errors="ignore").splitlines()[-25:]:
        print("  " + line)
    print("---------------------")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print(f"  backend crashed {CRASH_COUNT}x -> FE recorded each death + respawned,")
        print("  then it recovered -> fe-status healthy/un-latched, never hit the cap, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
