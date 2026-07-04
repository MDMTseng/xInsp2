"""
qa_overflow_block — the reintroduced overflow:"block" back-pressure policy.

Two dispatch groups, each max_parallel:1, queue_depth:2, fed by an identical
12ms-paced source (a DEDICATED emit thread — the back-pressure-tolerant target
block is meant for), while the inspect is a deliberately SLOW ~40ms drain. The
source out-runs the drain ~3x, so the queue is permanently full:

  * group "blk" (overflow:"block") back-pressures the source (parks it on a full
    queue) instead of dropping -> LOSSLESS: zero drops, processed == emitted.
  * group "drp" (overflow:"drop_oldest") drops the surplus -> processed < emitted,
    dropped > 0.  This is the contrast that makes "block is lossless" meaningful.

PHASE 1 (lossless + contrast): drive both for a window, stop the sources (joins
their emit threads -> final emitted counts), wait for the lanes to drain, then
assert blk is lossless and drp lost frames.

PHASE 2 (clean teardown while parked): a fresh backend, drive the block lane hot
so its source is parked on the full queue, then issue cmd:stop + close_project and
assert the backend responds within a bounded time. This exercises the cv_not_full
stop-wake: a parked producer MUST be woken (predicate keys off continuous) so it
degrades to a drop instead of hanging teardown (a missing wake would deadlock the
close on the still-parked source emit thread).

Run:  python examples/qa_overflow_block/driver.py   (Windows; backend + plugins built)
"""
from __future__ import annotations
import json, os, subprocess, sys, time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
WINDOW = 3.0


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_ob_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = str(iso)
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    return subprocess.Popen([str(BACKEND), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO), env=env)


def connect(port):
    for _ in range(80):
        try:
            c = Client(url=f"ws://127.0.0.1:{port}/", timeout=60); c.connect(); c.ping(); return c
        except Exception:
            time.sleep(0.5)
    return None


def _emitted(defval) -> int:
    """exchange_instance returns the plugin's get_def() JSON (dict or string)."""
    if isinstance(defval, str):
        try: defval = json.loads(defval)
        except Exception: return -1
    return int(defval.get("emitted", -1)) if isinstance(defval, dict) else -1


def _load(c):
    c.call("open_project", {"path": str(ROOT)})
    c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
    c.call("start", {"fps": 0})


def phase1(fails: list[str]) -> None:
    port = free_port(); proc = spawn(port)
    try:
        c = connect(port)
        if not c: fails.append("phase1: no connect"); return
        _load(c)
        # Start both dedicated emit threads (they emit ONLY after cmd:start above, so
        # every emitted frame lands in the continuous lane -> emitted==processed clean).
        c.call("exchange_instance", {"name": "src_blk", "cmd": {"command": "start"}})
        c.call("exchange_instance", {"name": "src_drp", "cmd": {"command": "start"}})

        n = defaultdict(int)
        end = time.time() + WINDOW
        while time.time() < end:
            try: ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception: continue
            if ev.get("name") == "run_result" and ev.get("data", {}).get("code") == 1:
                n[ev["data"].get("group")] += 1

        # Stop the sources: joins each emit thread. A parked block producer completes
        # its in-flight push as a worker frees a slot, then exits -> emitted is final.
        d_blk = c.call("exchange_instance", {"name": "src_blk", "cmd": {"command": "stop"}})
        d_drp = c.call("exchange_instance", {"name": "src_drp", "cmd": {"command": "stop"}})
        emit_blk, emit_drp = _emitted(d_blk), _emitted(d_drp)

        # Drain: keep counting run_results until the block lane has processed every
        # emitted frame (lossless => it must reach emit_blk), bounded by a deadline.
        drain_end = time.time() + 15.0
        while n["blk"] < emit_blk and time.time() < drain_end:
            try: ev = c._inbox_events.get(timeout=0.2)
            except Exception: continue
            if ev.get("name") == "run_result" and ev.get("data", {}).get("code") == 1:
                n[ev["data"].get("group")] += 1

        st = {g["name"]: g for g in (c.call("dispatch_stats").get("groups") or [])}
        drop_blk = int(st.get("blk", {}).get("dropped", 0))
        drop_drp = int(st.get("drp", {}).get("dropped", 0))
        c.call("stop"); c.call("close_project"); c.close()

        print(f"blk (block):       emitted={emit_blk} processed={n['blk']} dropped={drop_blk}")
        print(f"drp (drop_oldest): emitted={emit_drp} processed={n['drp']} dropped={drop_drp}")

        # --- LOSSLESS assertions for the block lane ---
        if emit_blk <= 0:
            fails.append(f"phase1: block source emitted={emit_blk} (nothing emitted?)")
        if drop_blk != 0:
            fails.append(f"phase1: block lane DROPPED {drop_blk} (must be 0 — back-pressure, not drop)")
        if emit_blk > 0 and n["blk"] != emit_blk:
            fails.append(f"phase1: block lane NOT lossless: processed {n['blk']} != emitted {emit_blk}")
        # --- CONTRAST: drop_oldest must actually lose frames under the same load ---
        if drop_drp < 1:
            fails.append("phase1: drop_oldest lane dropped nothing — load too light to prove contrast")
        if emit_drp > 0 and n["drp"] >= emit_drp:
            fails.append(f"phase1: drop_oldest processed {n['drp']} >= emitted {emit_drp} (expected loss)")
    except Exception as e:
        fails.append(f"phase1: exception: {e!r}")
    finally:
        try: proc.terminate(); proc.wait(10)
        except Exception: proc.kill()


def phase2(fails: list[str]) -> None:
    port = free_port(); proc = spawn(port)
    try:
        c = connect(port)
        if not c: fails.append("phase2: no connect"); return
        _load(c)
        # Drive the block lane hot and let it saturate so the source is parked on the
        # full queue (queue_depth:2, 12ms source vs 40ms drain).
        c.call("exchange_instance", {"name": "src_blk", "cmd": {"command": "start"}})
        time.sleep(1.0)   # by now the block queue is full and the source is parked

        # Teardown WHILE parked. If the cv_not_full stop-wake is missing, close_project
        # hangs forever joining the still-parked emit thread; here it must return fast.
        t0 = time.time()
        c.call("stop", timeout=20)
        c.call("close_project", timeout=20)
        dt = time.time() - t0
        c.close()
        print(f"phase2: stop+close_project while parked returned in {dt:.2f}s")
        if dt > 12.0:
            fails.append(f"phase2: teardown took {dt:.2f}s (> 12s bound) — parked producer not woken?")
    except Exception as e:
        # A hung teardown surfaces as a call timeout -> exception here.
        fails.append(f"phase2: exception (teardown likely hung): {e!r}")
    finally:
        try: proc.terminate(); proc.wait(10)
        except Exception: proc.kill()


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []
    phase1(fails)
    phase2(fails)
    if fails:
        for f in fails: print("  -", f)
        print("VERDICT: FAIL: " + "; ".join(fails))
        return 1
    print("VERDICT: PASS: overflow:block is lossless (zero drops, processed==emitted) while "
          "drop_oldest loses frames under the same load, and teardown while a producer is "
          "parked returns within a bounded time (cv_not_full stop-wake degrades to drop, no hang)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
