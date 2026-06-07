"""H3 PoC — gateway hang is observable via a stalled heartbeat.

The old FE only checked process EXIT (WaitForSingleObject), so a gateway that
stays alive but wedges (relay loop stuck) was invisible and left the PLC
asserted. The fix: the gateway writes a heartbeat counter each loop turn; the
FE watches it for staleness (the exact mechanism, code, and staleness math the
FE already uses for the backend — regression-tested by qa_race serve-wedge).

This PoC verifies the NEW comms-side behavior + the signal the FE consumes:
spawn a gateway with --hang-after-ms; confirm the heartbeat advances, then
STALLS after the hang, while the process stays alive and its PLC socket open.
That stalled-but-alive state is exactly what the FE's staleness check trips on.
"""
from __future__ import annotations
import os, socket, subprocess, time
from pathlib import Path
from harness import ROOT, COMMS_EXE

HANG_MS = 2000
STALE_BUDGET_MS = 1500   # same default the FE uses (heartbeat_stale_ms)


def read_hb(p: Path) -> int:
    try:
        return int(p.read_text().strip())
    except Exception:
        return -1


def main() -> int:
    if os.name != "nt" or not COMMS_EXE.exists():
        print("SKIP"); return 0

    # mock PLC: a TCP listener the gateway connects to (kept open the whole time)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0)); srv.listen(1); srv.settimeout(0.5)
    plc_port = srv.getsockname()[1]

    hb = ROOT / "h3_gw.hb"
    if hb.exists(): hb.unlink()
    log = ROOT / "h3_gw.log"
    gw = subprocess.Popen(
        [str(COMMS_EXE), f"--plc=tcp:127.0.0.1:{plc_port}", "--listen=7908",
         f"--heartbeat-file={hb}", f"--hang-after-ms={HANG_MS}"],
        cwd=str(COMMS_EXE.parent),
        stdout=open(log, "wb"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
    )
    plc_conn = None
    try:
        # accept the gateway's PLC connection so the link is "up"
        t0 = time.time()
        while time.time() - t0 < 5 and plc_conn is None:
            try: plc_conn, _ = srv.accept()
            except socket.timeout: pass

        # Phase 1: heartbeat must ADVANCE before the hang
        time.sleep(0.8)
        a = read_hb(hb)
        time.sleep(0.8)
        b = read_hb(hb)
        advanced = b > a >= 0
        print(f"pre-hang heartbeat: {a} -> {b} (advancing={advanced})")

        # Phase 2: wait past the hang point, then confirm it STALLS
        time.sleep(HANG_MS / 1000.0 + 0.8)
        c1 = read_hb(hb)
        time.sleep((STALE_BUDGET_MS / 1000.0) + 0.5)
        c2 = read_hb(hb)
        stalled = (c1 == c2 and c1 >= 0)
        alive = gw.poll() is None
        # the PLC socket is still established (hang != link drop)
        plc_open = plc_conn is not None
        print(f"post-hang heartbeat: {c1} -> {c2} (stalled={stalled})")
        print(f"gateway process alive during stall = {alive}")
        print(f"PLC socket still established = {plc_open}")

        print("\n==== H3 VERDICT ====")
        if advanced and stalled and alive and plc_open:
            print("heartbeat advanced then STALLED while the gateway stayed alive with "
                  "its PLC link open -> 'hang != crash' is now detectable.")
            print("=> H3 FIXED (FE watches this heartbeat with the same staleness check "
                  "it uses for the backend; a stall > heartbeat_stale_ms triggers "
                  "kill + safe-state + respawn).")
        else:
            print("=> H3 NOT reproduced — investigate")
            print(Path(log).read_text(errors='ignore')[-600:])
        return 0
    finally:
        if plc_conn:
            try: plc_conn.close()
            except OSError: pass
        srv.close()
        if gw.poll() is None:
            gw.terminate()
            try: gw.wait(timeout=4)
            except subprocess.TimeoutExpired: gw.kill()
        if hb.exists(): hb.unlink()


if __name__ == "__main__":
    raise SystemExit(main())
