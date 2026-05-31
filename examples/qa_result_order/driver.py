"""qa_result_order — arrival-ordered result emission under a parallel pool.

Under parallelism.dispatch_threads > 1, per-frame results (vars) are emitted as
each worker FINISHES by default ("completion"), so with uneven inspect times the
wire stream is out of frame order. parallelism.result_order:"arrival" makes the
backend replay them in frame-arrival order (compute still runs parallel; only
emission is gated). run_id is now assigned at dequeue, so it tracks arrival
order and the wire stream's run_id must be monotonic in arrival mode.

This driver runs the SAME uneven-timing script (every 5th frame slow) under both
modes with dispatch_threads=4, collecting the run_id of each `vars` message in
receive order. Asserts:
  * completion mode -> reordered (inversions > 0), proving the workload really
    does run concurrently and finish out of order under this pool, and
  * arrival mode    -> run_id strictly increasing (zero inversions) on that same
    reordering workload.
(The queue high-watermark is reported but not asserted: 4 workers drain the
timer faster than it fills, so concurrency shows up as out-of-order completions,
not queue depth.)

Run:  python driver.py

TODO(linux): backend compile (cl.exe) is Windows-only; SKIPs on non-nt.
"""
from __future__ import annotations

import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{SUF}"
COLLECT_S = 4.0


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout): return True
    except OSError:
        return False


def run_mode(proj: Path, port: int):
    """Run one project; return (run_ids_in_receive_order, high_watermark)."""
    blog = open(proj / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={port}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    run_ids: list[int] = []
    hwm = 0
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(port):
            if be.poll() is not None:
                raise SystemExit(f"FAIL: backend exited during boot rc={be.poll()}")
            time.sleep(0.2)
        with Client(url=f"ws://127.0.0.1:{port}/", timeout=60) as c:
            c.open_project(str(proj), timeout=300)
            c.compile_and_load(str(proj / "inspect.cpp"), timeout=180)
            c.call("start", {"fps": 120})   # push hard so the queue backs up
            t_end = time.time() + COLLECT_S
            while time.time() < t_end:
                v = c.next_vars(timeout=0.5)
                if not v:
                    continue
                rid = v.get("run_id")
                if isinstance(rid, int):
                    run_ids.append(rid)
            try:
                hwm = int(c.call("dispatch_stats").get("queue_depth_high_watermark", 0))
            except Exception:
                hwm = 0
            try: c.call("stop")
            except Exception: pass
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{port}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        try: be.wait(timeout=6)
        except subprocess.TimeoutExpired: be.kill()
        blog.close()
    return run_ids, hwm


def inversions(seq: list[int]) -> int:
    return sum(1 for a, b in zip(seq, seq[1:]) if b < a)


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend compile is Windows-only here")
        return 0
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")

    failures: list[str] = []

    arr_ids, arr_hwm = run_mode(ROOT / "arrival", 7877)
    arr_inv = inversions(arr_ids)
    print(f"[arrival]    frames={len(arr_ids)} high_watermark={arr_hwm} inversions={arr_inv}")

    comp_ids, comp_hwm = run_mode(ROOT / "completion", 7878)
    comp_inv = inversions(comp_ids)
    print(f"[completion] frames={len(comp_ids)} high_watermark={comp_hwm} inversions={comp_inv}")

    if len(arr_ids) < 30:
        failures.append(f"arrival: only {len(arr_ids)} frames - not enough to judge ordering")
    if comp_inv == 0:
        failures.append("completion mode did NOT reorder (0 inversions) - the workload didn't "
                        "exercise concurrent out-of-order completion, so the arrival result is "
                        "inconclusive (raise fps or the slow-frame ratio)")
    if arr_inv != 0:
        failures.append(f"arrival: run_id NOT monotonic in receive order ({arr_inv} inversions) "
                        f"- results were emitted out of arrival order")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures: print("  -", f)
    else:
        print("VERDICT: PASS")
        print(f"  arrival mode: {len(arr_ids)} frames, concurrent pool (hwm={arr_hwm}),")
        print(f"  zero out-of-order results. completion mode reordered ({comp_inv} inversions).")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
