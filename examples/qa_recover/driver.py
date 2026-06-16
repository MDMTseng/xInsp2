"""qa_recover — FE supervisor recover-and-clear regression (Phase G, #92).

fe_supervisor proves the FE's *give-up* path: a backend that crashes forever ->
safe-state -> respawn -> hit the cap -> stay safe. This proves the *happy exit*
the cap path can't: a backend that crashes a FEW times and then RECOVERS must
have its safe state CLEARED, and the FE must NOT hit the cap (test plan FE-E5 /
safety property SP4).

What it does
------------
1. Picks a fresh marker file and exports XI_QA_RECOVER_MARKER (inherited
   FE -> backend -> plugin). The `crash_then_heal` plugin crashes the backend its
   first `crash_count` (2) starts, counting in that file (which survives the
   process dying), then returns normally forever.
2. Launches `xinsp-fe.exe` on this project with `--autostart-fps`.
3. The backend crashes twice -> FE drives BackendExit safe-state + respawns each
   time. The 3rd backend instance is healthy -> FE logs CLEAR SAFE STATE.
4. Asserts from the FE log + state:
     - >=1 `ENTER SAFE STATE reason=BackendExit` (death detected, line driven safe),
     - >=1 `respawning backend`,
     - `CLEAR SAFE STATE` present (the recover transition — the whole point),
     - NO `RespawnLimitExceeded` (recovered before the cap),
     - the marker counted EXACTLY `crash_count` crashes,
     - the backend is up at the end and the FE is still running (didn't give up).
5. Stops the FE and asserts no orphan.

Run from this dir:  python driver.py

TODO(linux): xinsp-fe is Windows-only today. Skips on non-nt.
"""
from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE_SUFFIX}"
PORT = 7861
CRASH_COUNT = 2                       # must match instances/healer/instance.json
FE_LOG = ROOT / "fe.log"
BE_LOG = ROOT / "be.log"
MAX_WAIT_S = 120.0


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
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/roadmap/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release "
                 f"--target xinsp_fe xinsp_backend")
    if port_open():
        sys.exit(f"FAIL: something already listening on :{PORT}; pick a free port")

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
         f"--be-log={BE_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
        env=env,
    )
    print(f"[fe] launched pid={proc.pid} port={PORT} marker={marker}")

    failures: list[str] = []
    recovered = False
    try:
        # Wait until: the backend crashed crash_count times (marker), CLEAR is in
        # the log, and the backend is back up — or the FE gives up / times out.
        deadline = time.time() + MAX_WAIT_S
        while time.time() < deadline:
            if proc.poll() is not None:
                # The FE only self-exits on the cap path; that would be a failure
                # here (it should have recovered first).
                break
            log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
            if "reason=RespawnLimitExceeded" in log:
                break  # gave up — assertions below will flag it
            if (read_marker(marker) >= CRASH_COUNT
                    and "CLEAR SAFE STATE" in log
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

        n_backend_exit = sum(1 for ln in lines
                             if "ENTER SAFE STATE" in ln and "reason=BackendExit" in ln)
        if n_backend_exit < 1:
            failures.append("no 'ENTER SAFE STATE reason=BackendExit' — crash not detected / not driven safe")
        if not any("respawning backend" in ln for ln in lines):
            failures.append("FE never logged a respawn")
        if "CLEAR SAFE STATE" not in log:
            failures.append("no 'CLEAR SAFE STATE' — the recover-and-clear transition never happened (FE-E5)")
        if "reason=RespawnLimitExceeded" in log:
            failures.append("FE hit the respawn cap — it gave up instead of recovering")

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
        print(f"  backend crashed {CRASH_COUNT}x -> FE drove safe-state + respawned,")
        print("  then it recovered -> FE CLEARED safe state, never hit the cap, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
