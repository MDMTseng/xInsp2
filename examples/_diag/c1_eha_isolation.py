"""C1 PoC (v2) — controlled comparison.

A naive "handles climb after crash" is misleading because EVERY run grows the
pool by ~1 (last-run image retention). To isolate a CRASH-SPECIFIC unwind
leak we measure two slopes on the SAME backend:

  PHASE A (happy): limit=8, run frame_00 (ok) x M  -> baseline slope
  PHASE B (crash): exchange set_threshold=0 so any blob count > 0 null-derefs,
                   run frame_00 x M               -> crash slope

If SEH skips RAII unwind under /EHsc, each crash leaks `mask`(pool) + `src`
(adopted) that a happy return would release, so crash-slope > happy-slope.
If unwind is sound, the two slopes match (the growth is just retention).

Also reports: did the backend stay alive (isolation), and did crash runs
actually surface as non-ok.
"""
from __future__ import annotations
import sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client, ProtocolError  # noqa: E402

PROJ = REPO_ROOT / "examples" / "crash_recovery"
FRAMES = PROJ / "frames"
FRAME = str(FRAMES / "frame_00.png")   # 5 blobs
PORT = 7901
M = 15


def handles(c: Client) -> int:
    return c.image_pool_stats()["total"]["handles"]


def try_run(c: Client, fp: str):
    try:
        r = c.run(frame_path=fp, timeout=15.0)
        return "ok", r.value("count", "?")
    except ProtocolError as e:
        return "error", str(e)[:60]
    except Exception as e:
        return type(e).__name__, str(e)[:60]


def measure_slope(c: Client, label: str, n: int):
    pts = []
    alive = True
    nonok = 0
    for i in range(n):
        st, detail = try_run(c, FRAME)
        # the framework ABSORBS a plugin AV and still returns a record, so
        # run()=="ok"; the crash shows up as the missing 'count' -> -1.
        crashed = (st != "ok") or (str(detail) == "-1")
        if crashed:
            nonok += 1
        try:
            c.ping()
        except Exception:
            alive = False
        time.sleep(0.1)
        try:
            h = handles(c)
        except Exception as e:
            alive = False
            print(f"  {label}[{i}] stats failed: {e}")
            break
        pts.append(h)
        print(f"  {label}[{i:2d}] run={st:10s} count={detail} handles={h}")
    slope = (pts[-1] - pts[0]) / max(1, len(pts) - 1) if len(pts) >= 2 else 0.0
    return slope, alive, nonok, pts


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP: backend exe missing"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} already in use"); return 1
    log = ROOT / "c1_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: backend never ready"); print(read_log(log)[-1500:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            try_run(c, FRAME)  # warm
            time.sleep(0.2)

            print("PHASE A (happy, limit=8):")
            slope_a, alive_a, nonok_a, _ = measure_slope(c, "A", M)

            print("set_threshold=0 (force crash on any blob)...")
            c.exchange_instance("cnt", {"command": "set_threshold", "value": 0})
            time.sleep(0.2)

            print("PHASE B (crash, limit=0):")
            slope_b, alive_b, nonok_b, _ = measure_slope(c, "B", M)

            print("\n==== C1 VERDICT ====")
            print(f"isolation: phaseA alive={alive_a}, phaseB alive={alive_b}")
            print(f"crash runs surfaced non-ok: {nonok_b}/{M} (phaseA non-ok={nonok_a}/{M})")
            print(f"happy slope = {slope_a:.2f} handles/run")
            print(f"crash slope = {slope_b:.2f} handles/run")
            if not (alive_a and alive_b):
                print("=> C1 claim-1 (isolation failure) CONFIRMED: backend died")
            else:
                print("=> SEH isolation HOLDS under /EHsc (backend survived all)")
            if nonok_b == 0:
                print("   NOTE: crash never surfaced -- leak test INCONCLUSIVE.")
            else:
                extra = slope_b - slope_a
                print(f"   crash-specific extra leak = {extra:.2f} handles/crash")
                if extra >= 0.8:
                    print("=> C1-b (RAII unwind slot leak on crash) CONFIRMED")
                elif extra > 0.2:
                    print("=> C1-b PARTIAL: small extra leak, investigate")
                else:
                    print("=> C1-b NOT reproduced: crash slope ~= happy slope")
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
