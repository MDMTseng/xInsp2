"""Phase C comm-reorder template smoke — prove the comms-as-plugin reorder: feed
each stream's sends OUT of seq order; the plugin must FLUSH them in order, and
keep streams independent (the parallel-dispatch "順序問題"). Driver drives the
comm plugin directly via exchange_instance (no script needed).
"""
from __future__ import annotations
import sys
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "comm_proj"
PORT = 7914


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "comm_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            def send(stream, seq, payload):
                c.exchange_instance("comm", {"op": "send", "stream": stream,
                                             "seq": seq, "payload": payload})
            # Stream A out of order: 0,2,1,3 ; Stream B interleaved out of order.
            send("A", 0, "a0"); send("A", 2, "a2"); send("B", 1, "b1")
            send("A", 1, "a1"); send("B", 0, "b0"); send("A", 3, "a3")
            a = c.exchange_instance("comm", {"op": "drain", "stream": "A"})
            b = c.exchange_instance("comm", {"op": "drain", "stream": "B"})
        fa = a.get("flushed") if isinstance(a, dict) else None
        fb = b.get("flushed") if isinstance(b, dict) else None
        print("\n==== COMM REORDER ====")
        print(f"A flushed={fa!r}  B flushed={fb!r}")
        passed = (fa == "a0,a1,a2,a3" and fb == "b0,b1")
        print("VERDICT:", "PASS" if passed else "FAIL")
        if not passed:
            print(read_log(log)[-800:])
        return 0 if passed else 1
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
