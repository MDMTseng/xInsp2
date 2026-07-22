"""qa_pack_order — qa_result_order's arrival-ordered pattern, PACK-ONLY (U3).

This is the pack-plane successor to qa_result_order (docs/new_gen/17; flips
parity-matrix row C3). Same uneven-timing workload (every 5th frame slow,
dispatch_threads=4), but the per-frame output is a script-built sealed pack
(xi::ScriptPackBuilder) pushed to the `expose` sink via xi::use("expose").push()
— no xi::Record anywhere in the script.

Ordering per the doc-17 contract:
  * DELIVERY order is the staging envelope's: push() on a declared sink is
    staged and flushed inside the per-lane emit gate, so the wire stream
    follows result_order (arrival = frame order, completion = finish order).
  * IDENTITY is producer-stamped: the host never stamps a sealed pack, so the
    script stamps `$seq = xi::run_id()` itself before seal — the SAME
    arrival/run id the Record-era host stamp injected at flush.

Doctrine probe (doc 17 §b): each frame also calls use("expose").process(pack)
— on a declared ordered sink that is REJECTED (-5 -> empty pack), because the
sink feed is push(). The rejection is recorded in the pushed pack
(`probe_rejected`) and asserted on every frame.

Asserts:
  * completion mode -> wire seq reordered (inversions > 0): the workload
    really does finish out of order under this pool;
  * arrival mode    -> wire seq strictly increasing (zero inversions) on that
    same workload — staged pack flush delivers in frame order;
  * every frame carries seq > 0 (xi::run_id() actually flowed — an older-host
    degrade would read 0) and probe_rejected == 1 (process-on-sink rejected).

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
sys.path.insert(0, str(ROOT.parent / "lib"))   # qa/lib (shared xex1 decoder)
from xinsp2 import Client  # noqa: E402
from xex1 import collect_frames, subscribe  # noqa: E402
from ports import backend_exe  # noqa: E402

BE = backend_exe()
COLLECT_S = 4.0


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout): return True
    except OSError:
        return False


def _harvest(c, rows: list[tuple[int, int]]) -> None:
    """Decode every XEX1 frame queued on the binary inbox (receive order) and
    append (wire seq = producer-stamped $seq = run_id, probe_rejected)."""
    for fr in collect_frames(c):
        if fr.get("channel") != "order":
            continue
        seq = fr.get("seq")
        if not isinstance(seq, int):
            continue
        pr = fr.get("values", {}).get("probe_rejected")
        rows.append((seq, pr if isinstance(pr, int) else -1))


def run_mode(proj: Path, port: int):
    """Run one project; return rows [(seq, probe_rejected)] in receive order."""
    blog = open(proj / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={port}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    rows: list[tuple[int, int]] = []
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(port):
            if be.poll() is not None:
                raise SystemExit(f"FAIL: backend exited during boot rc={be.poll()}")
            time.sleep(0.2)
        with Client(url=f"ws://127.0.0.1:{port}/", timeout=60) as c:
            c.open_project(str(proj), timeout=300)
            c.compile_and_load(str(proj / "inspect.cpp"), timeout=180)
            subscribe(c, ["order"])         # gate expose to push the "order" channel
            c.drain_binary()                # drop any frames from before the window
            c.call("start", {"fps": 120})   # push hard so the queue backs up
            t_end = time.time() + COLLECT_S
            while time.time() < t_end:
                time.sleep(0.05)
                _harvest(c, rows)
            try: c.call("stop")
            except Exception: pass
            time.sleep(0.2)
            _harvest(c, rows)               # drain any in-flight frames after stop
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
    return rows


def inversions(seq: list[int]) -> int:
    return sum(1 for a, b in zip(seq, seq[1:]) if b < a)


def main() -> int:
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")

    failures: list[str] = []

    arr_rows = run_mode(ROOT / "arrival", 7885)
    arr_ids = [s for s, _ in arr_rows]
    arr_inv = inversions(arr_ids)
    print(f"[arrival]    frames={len(arr_rows)} inversions={arr_inv}")

    comp_rows = run_mode(ROOT / "completion", 7886)
    comp_ids = [s for s, _ in comp_rows]
    comp_inv = inversions(comp_ids)
    print(f"[completion] frames={len(comp_rows)} inversions={comp_inv}")

    if len(arr_rows) < 30:
        failures.append(f"arrival: only {len(arr_rows)} frames - not enough to judge ordering")
    if comp_inv == 0:
        failures.append("completion mode did NOT reorder (0 inversions) - the workload didn't "
                        "exercise concurrent out-of-order completion, so the arrival result is "
                        "inconclusive (raise fps or the slow-frame ratio)")
    if arr_inv != 0:
        failures.append(f"arrival: wire seq NOT monotonic in receive order ({arr_inv} inversions) "
                        f"- staged pack pushes were flushed out of frame order")

    all_rows = arr_rows + comp_rows
    if any(s <= 0 for s, _ in all_rows):
        failures.append("some frames carried $seq <= 0 - xi::run_id() did not flow "
                        "(producer-stamped host arrival id missing)")
    bad_probe = sum(1 for _, pr in all_rows if pr != 1)
    if bad_probe:
        failures.append(f"{bad_probe} frames did NOT record the process-on-sink rejection "
                        f"(probe_rejected != 1) - doc 17 (b) doctrine violated")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures: print("  -", f)
    else:
        print("VERDICT: PASS")
        print(f"  arrival: {len(arr_rows)} frames, zero out-of-order pack deliveries;")
        print(f"  completion reordered ({comp_inv} inversions); process-on-sink "
              f"rejected on every frame.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
