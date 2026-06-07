"""Smoke: confirm the backend's script-compile path (vcvars/cl.exe) works.

Spawns a backend on a private port pointed at qa_run_result (a script-only
project), waits for 'autostart: ready' (which only appears AFTER cl.exe
cold-compiles inspect.cpp), then attaches a client and runs one frame.
PASS => the compile path used by C1/H1/H2 is live.
"""
from __future__ import annotations
import sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

PROJ = REPO_ROOT / "examples" / "qa_run_result"
PORT = 7900


def main() -> int:
    if not BACKEND_EXE.exists():
        print(f"SKIP: backend exe missing at {BACKEND_EXE}")
        return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} already in use")
        return 1
    log = ROOT / "smoke_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        t0 = time.time()
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print(f"FAIL: never reached 'autostart: ready' in {PORT_UP_BUDGET}s")
            print("---- log tail ----")
            print(read_log(log)[-2000:])
            return 1
        dt = time.time() - t0
        print(f"PASS: compiled + ready in {dt:.1f}s")
        # quick client round-trip
        sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
        from xinsp2 import Client
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            st = c.status()
            print(f"PASS: client connected, status keys = {sorted(st.keys())[:8]}")
            stats = c.image_pool_stats()
            print(f"INFO: image_pool_stats = {stats}")
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
