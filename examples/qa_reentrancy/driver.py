"""qa_reentrancy — proves the declared-reentrancy safety model for the parallel
dispatch pool (burst frame parallelism).

The backend already has a parallel dispatch pool (project.json
parallelism.dispatch_threads). The risk: N workers re-entering the SAME stateful
instance's process() concurrently. The safety model: a plugin is serialized
per-instance by the host UNLESS its plugin.json declares "reentrant": true.

This driver runs ONE continuous session with dispatch_threads=4 and two probe
instances poked every frame:
  * "serial"   -> concurrency_probe     (no reentrant flag) — must be serialized
  * "parallel" -> concurrency_probe_rt  ("reentrant": true) — runs concurrently
Each probe reports the max number of workers seen inside its own process() at
once. Asserts:
  * max(maxc_serial)   == 1   AND  max(overlaps_serial) == 0   (host lock holds)
  * max(maxc_parallel) >= 2                                    (reentrant => parallel)

A maxc_serial > 1 would mean the host let two workers into a non-reentrant
instance at once — the exact race the model exists to prevent.

Run:  python driver.py

TODO(linux): backend plugin compile (cl.exe) is Windows-only here; SKIPs on non-nt.
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
PORT = 7869
FPS = 200            # well above the 8ms probe sleep so the queue fills + workers overlap
COLLECT_S = 4.0


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout): return True
    except OSError:
        return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend plugin compile is Windows-only here")
        return 0
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")
    if port_open(PORT):
        sys.exit(f"FAIL: something already on :{PORT}")

    blog = open(ROOT / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={PORT}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    failures: list[str] = []
    maxc_serial = maxc_parallel = 0
    overlaps_serial = 0
    nframes = 0
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(PORT):
            time.sleep(0.2)
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60) as c:
            c.open_project(str(ROOT), timeout=300)
            c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=180)
            c.call("start", {"fps": FPS})

            t_end = time.time() + COLLECT_S
            while time.time() < t_end:
                v = c.next_vars(timeout=0.5)
                if not v:
                    continue
                items = {it["name"]: it.get("value") for it in v.get("items", [])}
                if "maxc_serial" in items:
                    nframes += 1
                    maxc_serial    = max(maxc_serial,    int(items.get("maxc_serial") or 0))
                    maxc_parallel  = max(maxc_parallel,  int(items.get("maxc_parallel") or 0))
                    overlaps_serial = max(overlaps_serial, int(items.get("overlaps_serial") or 0))
            try:
                c.call("stop")
            except Exception:
                pass

        print(f"[result] frames={nframes} maxc_serial={maxc_serial} "
              f"overlaps_serial={overlaps_serial} maxc_parallel={maxc_parallel}")

        if nframes < 10:
            failures.append(f"only {nframes} frames seen — pool/continuous mode didn't run enough")
        if maxc_serial != 1:
            failures.append(f"non-reentrant instance reached concurrency {maxc_serial} "
                            f"(expected 1) — the per-instance lock did NOT hold")
        if overlaps_serial != 0:
            failures.append(f"non-reentrant instance saw {overlaps_serial} overlaps (expected 0)")
        if maxc_parallel < 2:
            failures.append(f"reentrant instance only reached concurrency {maxc_parallel} "
                            f"(expected >= 2) — reentrant flag didn't enable parallelism "
                            f"(or the machine couldn't overlap; try raising FPS/COLLECT_S)")
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        try: be.wait(timeout=5)
        except subprocess.TimeoutExpired: be.kill()
        blog.close()

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures: print("  -", f)
    else:
        print("VERDICT: PASS")
        print("  non-reentrant instance stayed serialized (maxc=1) under a 4-thread")
        print("  pool; reentrant instance ran concurrently (maxc>=2). Safe by default.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
