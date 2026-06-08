"""Parallel-comm reorder test — comm ordering under REAL parallel dispatch. A
source bursts 12 frames (each with a seq) into a max_parallel=4 lane; the script
random-sleeps per frame so the 4 workers COMPLETE out of order, then sends each
to the comm plugin tagged with its seq. comm reorders by seq -> the output stream
is ordered despite the scrambled arrivals. Prints arrivals (scrambled) vs flushed
(ordered) and asserts flushed == p0..p11.
"""
from __future__ import annotations
import sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "parallel_comm_proj"
PORT = 7916
N = 12


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "parallel_comm_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp",
                                     "--autostart-fps=-1"])   # trigger-only continuous: lanes up
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            emit = c.exchange_instance("src", {"n": N})
            print("emitted:", emit)
            # poll the comm drain until all N have flushed (or time out)
            flushed = arrivals = ""
            deadline = time.time() + 20
            while time.time() < deadline:
                d = c.exchange_instance("comm", {"op": "drain", "stream": "S"})
                flushed = d.get("flushed", "") if isinstance(d, dict) else ""
                arrivals = d.get("arrivals", "") if isinstance(d, dict) else ""
                if flushed.count("p") >= N:
                    break
                time.sleep(0.2)
        expect = ",".join(f"p{i}" for i in range(N))
        print("\n==== PARALLEL COMM REORDER (4 threads, random sleep) ====")
        print("arrivals (seq order received):", arrivals)
        print("flushed  (sent on, reordered):", flushed)
        scrambled = arrivals != ",".join(str(i) for i in range(N))
        passed = (flushed == expect)
        print(f"out-of-order arrivals: {scrambled}")
        print("VERDICT:", "PASS" if passed else "FAIL")
        if not passed:
            print(read_log(log)[-800:])
        return 0 if passed else 1
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
