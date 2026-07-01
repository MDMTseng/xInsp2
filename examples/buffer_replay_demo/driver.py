"""buffer_replay_demo driver — exercises the `cache` plugin (BufferReplay):
capture-into-ring + replay, the plugin-side record/replay store.

The `cache` plugin buffers each incoming record (image + meta) in a bounded
ring (capacity N) and re-emits a buffered one on an exchange command — the
HDevelop-style hot-param re-inspect loop: buffer a frame, tune a Param,
"replay" to re-inspect the SAME frame with no camera re-grab.

This driver asserts the plugin CONTRACT end-to-end through the real backend:
  1. ring is bounded to capacity (fire > capacity, count stays == capacity)
  2. HOT-PARAM RE-INSPECT: replay the SAME buffered frame under two different
     `thresh` values and confirm over_count moves the right way — proving the
     replay re-runs inspect on the buffered frame with the new param, no re-grab
  3. replay frames are tagged replayed / source=="buffer" (the guard holds)
  4. clear empties the ring

Observation uses the plugin's own get_def() count + the expose "runs" channel
(NOT VARs, which the generic client no longer decodes).

Run from this dir:  python driver.py
"""
from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

from xinsp2 import Client

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xex1 import subscribe, pull_latest   # noqa: E402

ROOT = Path(__file__).parent.resolve()
INSPECT_CPP = ROOT / "inspect.cpp"
BACKEND_EXE = (ROOT.parents[1] / "backend" / "build" / "Release" / "xinsp-backend.exe")

# Private port: the default 7823 may be held by a running VS Code extension
# (single-client backend), so this self-contained driver runs its own backend
# on a dedicated port and connects there.
PORT = 7911
URL = f"ws://127.0.0.1:{PORT}/"

FIRE = 20   # more than capacity, to prove the ring bound


def get_def(c: Client) -> dict:
    d = c.exchange_instance("buffer", {"command": "__stat__"})  # unknown -> get_def()
    return d if isinstance(d, dict) else {}


def wait_stable_count(c: Client, timeout=8.0) -> int:
    """Poll the ring size until it stops changing (detached inspects drained)."""
    t0 = time.monotonic()
    prev, stable = -999, 0
    while time.monotonic() - t0 < timeout:
        n = int(get_def(c).get("count", -1))
        stable = stable + 1 if n == prev else 0
        prev = n
        if stable >= 2:
            return n
        time.sleep(0.1)
    return prev


def replay_at_thresh(c: Client, thr: int, timeout=6.0) -> dict:
    """Set thresh, replay the last buffered frame, and return the fresh 'runs'
    result for that replay (polls until the frame carries our thresh)."""
    c.set_param("thresh", thr)
    c.exchange_instance("buffer", {"command": "replay_last", "n": 1})
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout:
        fr = pull_latest(c, "runs")
        if fr and int(fr["values"].get("thresh", -1)) == thr:
            return fr["values"]
        time.sleep(0.05)
    return (pull_latest(c, "runs") or {}).get("values", {})


def main() -> int:
    if not BACKEND_EXE.exists():
        print(f"FAIL: backend exe not found: {BACKEND_EXE}")
        return 1

    proc = subprocess.Popen([str(BACKEND_EXE), f"--port={PORT}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.8)
    fails = 0

    def check(cond, msg):
        nonlocal fails
        print(("  OK  " if cond else "  BAD ") + msg)
        if not cond:
            fails += 1

    try:
        with Client(url=URL) as c:
            c.open_project(str(ROOT), timeout=180)
            c.compile_and_load(str(INSPECT_CPP), timeout=180)
            subscribe(c, ["runs"])
            print("[driver] project + inspect loaded, subscribed to 'runs'")

            # take control: stop the free-running source, empty the ring.
            c.exchange_instance("src", {"command": "stop"})
            c.exchange_instance("buffer", {"command": "clear"})
            time.sleep(0.2)
            cap = int(get_def(c).get("capacity", -1))
            print(f"[driver] capacity = {cap}, ring cleared")

            # 1. fire K>capacity frames; each inspect captures into the ring.
            c.exchange_instance("src", {"command": "fire", "n": FIRE})
            n = wait_stable_count(c)
            print(f"[driver] fired {FIRE} -> ring count = {n}")
            check(n == cap, f"ring bounded to capacity: count({n}) == capacity({cap})")

            # 2. HOT-PARAM re-inspect of the SAME buffered frame.
            lo = replay_at_thresh(c, 10)     # low thresh -> many pixels over
            hi = replay_at_thresh(c, 250)    # high thresh -> few pixels over
            print(f"[driver] replay thresh=10  -> {lo}")
            print(f"[driver] replay thresh=250 -> {hi}")
            check(lo.get("replayed") is True and lo.get("source") == "buffer",
                  "replay tagged replayed / source=='buffer'")
            check("over" in lo and "over" in hi and lo["over"] > hi["over"],
                  f"same frame, over_count tracks thresh: over@10({lo.get('over')}) > over@250({hi.get('over')})")

            # 3. replayed frames are not re-buffered (emitter=='buffer' guard).
            check(int(get_def(c).get("count", -1)) == n,
                  "replays did not grow the ring (no re-buffer)")

            # 4. clear empties the ring.
            c.exchange_instance("buffer", {"command": "clear"})
            check(int(get_def(c).get("count", -1)) == 0, "clear empties the ring")

        print("\nRESULT:", "PASS" if fails == 0 else f"FAIL ({fails})")
        return 0 if fails == 0 else 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
