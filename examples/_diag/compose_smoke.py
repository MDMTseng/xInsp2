"""Composition capstone — camera template x2 -> collector (combining emitter) ->
inspection -> sort comm, all on a 4-thread lane. The driver captures frame N on
both cameras; each raw frame drives a collection run that feeds the collector;
when a pair completes the collector emits a correlated frame that drives an
inspection run, which random-sleeps then sends to comm tagged by frame number.
comm reorders -> the PLC stream is frame-ordered (f0..fK-1) despite the parallel
out-of-order completion. Exercises Emitter (camera + collector), pull-by-id
correlation, and the sort comm together.
"""
from __future__ import annotations
import sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "compose_proj"
PORT = 7917
K = 8


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "compose_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp",
                                     "--autostart-fps=-1"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            for n in range(K):
                c.exchange_instance("cam_left", {"frame": n})
                c.exchange_instance("cam_right", {"frame": n})
            flushed = arrivals = ""
            deadline = time.time() + 25
            while time.time() < deadline:
                d = c.exchange_instance("comm", {"op": "drain", "stream": "S"})
                flushed = d.get("flushed", "") if isinstance(d, dict) else ""
                arrivals = d.get("arrivals", "") if isinstance(d, dict) else ""
                if flushed.count("f") >= K:
                    break
                time.sleep(0.2)
        expect = ",".join(f"f{i}" for i in range(K))
        print("\n==== COMPOSE: cam x2 -> collector -> inspect -> comm ====")
        print("arrivals (frame order received):", arrivals)
        print("flushed  (reordered by frame):  ", flushed)
        passed = (flushed == expect)
        print("VERDICT:", "PASS" if passed else "FAIL")
        if not passed:
            print(read_log(log)[-1000:])
        return 0 if passed else 1
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
