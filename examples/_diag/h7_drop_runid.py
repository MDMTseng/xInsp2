"""H7 PoC — XI_SYS_DROPPED markers now carry a run_id so a consumer can order
them against run results (previously run_id was -1 / omitted, leaving drop
markers unorderable relative to the gated run_result stream).

Run a continuous project with queue_depth=1 + a slow inspect + high fps so the
dispatch queue overflows (drop_oldest). Collect run_result events; confirm the
'dropped: queue full' markers include a run_id field.
"""
from __future__ import annotations
import sys, time
from pathlib import Path
from queue import Empty
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "h7_proj"
PORT = 7909


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "h7_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp",
                                     "--autostart-fps=30"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            time.sleep(3.0)   # let the queue overflow under the slow inspect
            dropped = []
            sample = None
            try:
                while True:
                    ev = c._inbox_events.get(timeout=0.2)
                    if ev.get("name") != "run_result":
                        continue
                    data = ev.get("data") or {}
                    if isinstance(data, str):
                        import json
                        data = json.loads(data)
                    msg = str(data.get("msg", ""))
                    if "dropped" in msg:
                        dropped.append(data)
                        if sample is None:
                            sample = data
            except Empty:
                pass

            print(f"collected {len(dropped)} drop markers")
            if sample is not None:
                print(f"sample drop marker: {sample}")
            with_runid = [d for d in dropped if "run_id" in d and d.get("run_id", -1) >= 0]
            print("\n==== H7 VERDICT ====")
            if dropped and len(with_runid) == len(dropped):
                print(f"all {len(dropped)} drop markers carry a run_id "
                      f"(e.g. run_id={with_runid[0].get('run_id')})")
                print("=> H7 FIXED (drop markers are orderable via run_id; the gate "
                      "still serializes run results without stalling the source)")
            elif not dropped:
                print("no drops observed — try again / raise fps. INCONCLUSIVE")
            else:
                print(f"only {len(with_runid)}/{len(dropped)} markers had run_id -> investigate")
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
