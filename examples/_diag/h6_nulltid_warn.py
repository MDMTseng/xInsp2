"""H6 PoC — the bus warns when a source emits a NULL trigger id under a
correlating policy (all_required / leader_followers), instead of silently
producing a dead pipeline (events that never complete).

Open a project with all_required + two null_pulse sources, poke each to emit a
NULL-tid trigger, then check the backend log for the one-time warning.
"""
from __future__ import annotations
import sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "h6_proj"
PORT = 7910


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "h6_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            for inst in ("pa", "pb"):
                try:
                    c.exchange_instance(inst, {"command": "pulse"})
                except Exception as e:
                    print(f"exchange {inst}: {type(e).__name__}: {str(e)[:80]}")
            time.sleep(0.5)

        txt = read_log(log)
        warned = "NULL trigger id under the all_required" in txt
        print("\n==== H6 VERDICT ====")
        for line in txt.splitlines():
            if "NULL trigger id" in line:
                print("warning:", line.strip())
        if warned:
            print("=> H6 FIXED (NULL-tid-under-correlating-policy now warns instead "
                  "of a silent dead pipeline)")
        else:
            print("=> H6 warning not seen — check emit_trigger wiring / log")
            print(txt[-800:])
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
