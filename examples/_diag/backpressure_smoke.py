"""Phase B-2 back-pressure proof — emit_dispatch REJECTS on a full lane (never a
silent drop). The source bursts 200 emit_dispatch in one exchange into a tiny
lane (queue_depth=2); the producer outpaces the worker, so many calls return 0.
The plugin reports accepted/rejected; we assert rejected>0 (back-pressure
engaged) and accepted>=1 (some got through).

Runs in trigger-only continuous (--autostart-fps=-1) so the lane exists and
emit_dispatch routes through enqueue_dispatch_ rather than the one-shot path.
"""
from __future__ import annotations
import sys
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "backpressure_proj"
PORT = 7913


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "backpressure_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp",
                                     "--autostart-fps=-1"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            resp = c.exchange_instance("src", {})    # bursts 200; returns the split
        acc = resp.get("accepted") if isinstance(resp, dict) else None
        rej = resp.get("rejected") if isinstance(resp, dict) else None
        print("\n==== BACK-PRESSURE ====")
        print(f"accepted={acc} rejected={rej}")
        passed = (isinstance(acc, int) and isinstance(rej, int)
                  and rej > 0 and acc >= 1 and acc + rej == 200)
        print("VERDICT:", "PASS" if passed else "FAIL")
        if not passed:
            print(read_log(log)[-800:])
        return 0 if passed else 1
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
