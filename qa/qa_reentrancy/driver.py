"""qa_reentrancy — proves the declared-reentrancy safety model for the parallel
dispatch pool (burst frame parallelism).

The backend already has a parallel dispatch pool (project.json
parallelism.dispatch_threads). The risk: N workers re-entering the SAME stateful
instance's process() concurrently. The safety model: a plugin is serialized
per-instance by the host UNLESS its plugin.json declares "reentrant": true.

This driver runs ONE continuous session with dispatch_threads=4 and three probe
instances poked every frame:
  * "serial"   -> concurrency_probe     (no reentrant flag) — must be serialized
  * "parallel" -> concurrency_probe_rt  ("reentrant": true) — runs concurrently
  * "capped"   -> concurrency_probe_rt  (reentrant, instance.json
                  max_concurrency=1)   — reentrant but the per-instance cap
                  holds it to 1 (per-instance pool sizing)
Each probe reports the max number of workers seen inside its own process() at
once. Asserts:
  * max(maxc_serial)   == 1   AND  max(overlaps_serial) == 0   (host lock holds)
  * max(maxc_parallel) >= 2                                    (reentrant => parallel)
  * max(maxc_capped)   == 1                                    (cap=1 limits a
                                                                reentrant instance)

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
sys.path.insert(0, str(REPO_ROOT / "qa" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = backend_exe()
PORT = free_port()  # ephemeral (was 7869); no squatter cross-talk
FPS = 200            # well above the 8ms probe sleep so the queue fills + workers overlap
COLLECT_S = 4.0


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout): return True
    except OSError:
        return False


def main() -> int:
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")
    if port_open(PORT):
        sys.exit(f"FAIL: something already on :{PORT}")

    blog = open(ROOT / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={PORT}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    failures: list[str] = []
    maxc_serial = maxc_parallel = maxc_capped = 0
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

            # Let frames flow through all three instances; each probe accumulates
            # its own max-concurrency counter internally. No per-frame wire read —
            # VAR/vars was removed; we pull the totals afterwards via exchange.
            time.sleep(COLLECT_S)
            try:
                c.call("stop")
            except Exception:
                pass

            def stats(name: str) -> dict:
                r = c.exchange_instance(name, {"command": "stats"})
                if isinstance(r, str):
                    import json
                    r = json.loads(r)
                return r if isinstance(r, dict) else {}

            ss, sp, sc = stats("serial"), stats("parallel"), stats("capped")
            nframes         = int(ss.get("calls") or 0)
            maxc_serial     = int(ss.get("maxc") or 0)
            overlaps_serial = int(ss.get("overlaps") or 0)
            maxc_parallel   = int(sp.get("maxc") or 0)
            maxc_capped     = int(sc.get("maxc") or 0)

        print(f"[result] frames={nframes} maxc_serial={maxc_serial} "
              f"overlaps_serial={overlaps_serial} maxc_parallel={maxc_parallel} "
              f"maxc_capped={maxc_capped}")

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
        if maxc_capped != 1:
            failures.append(f"capped instance (max_concurrency=1) reached concurrency "
                            f"{maxc_capped} (expected 1) — the per-instance cap did NOT hold")
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
        print("  pool; reentrant instance ran concurrently (maxc>=2); reentrant instance")
        print("  with max_concurrency=1 was capped back to 1. Safe + sizable by default.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
