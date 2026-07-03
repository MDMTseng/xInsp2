"""
Runtime knobs: live set_timer_fps / set_process_priority + project.json runtime.

A source-less project whose synthetic timer drives every run, so run_result rate ==
timer rate. Asserts:
  - project.json runtime.timer_fps (8) is applied on open: a bare start (no fps)
    runs at ~8/s;
  - set_timer_fps {fps:20} LIVE bumps the rate to ~20/s without restarting;
  - set_timer_fps {fps:0} (trigger-only) drops it to ~0 (no source);
  - set_process_priority accepts a valid class and rejects a bogus one.

Run:  python examples/qa_runtime_settings/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "0")) or free_port()


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_rt_iso"
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


def rate(c, window):
    """Drain run_result events for `window` s, return events/sec."""
    while True:
        try: c._inbox_events.get_nowait()
        except Exception: break
    n = 0; t0 = time.time(); end = t0 + window
    while time.time() < end:
        try: ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
        except Exception: break
        if ev.get("name") == "run_result": n += 1
    return n / (time.time() - t0)


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)

        c.call("start", {})        # NO fps -> should use runtime.timer_fps (8)
        time.sleep(0.5)
        r8 = rate(c, 2.0)
        print(f"runtime.timer_fps=8, bare start -> {r8:.1f}/s")
        if not (5.0 <= r8 <= 11.0):
            fails.append(f"runtime.timer_fps not applied: {r8:.1f}/s (want ~8)")

        c.call("set_timer_fps", {"fps": 20})   # live bump
        time.sleep(0.5)
        r20 = rate(c, 2.0)
        print(f"set_timer_fps 20 (live) -> {r20:.1f}/s")
        if not (14.0 <= r20 <= 26.0):
            fails.append(f"live set_timer_fps 20 -> {r20:.1f}/s (want ~20)")

        c.call("set_timer_fps", {"fps": 0})    # trigger-only: no source -> ~0
        time.sleep(0.6)
        r0 = rate(c, 1.5)
        print(f"set_timer_fps 0 (trigger-only, no source) -> {r0:.1f}/s")
        if r0 > 2.0:
            fails.append(f"trigger-only still ticked: {r0:.1f}/s")

        try:
            c.call("set_process_priority", {"class": "above"})   # valid -> ok
            print("set_process_priority above -> ok")
        except Exception as e:
            fails.append(f"set_process_priority 'above' rejected: {e}")
        try:
            c.call("set_process_priority", {"class": "bogus"})   # invalid -> should raise
            fails.append("set_process_priority 'bogus' was accepted (should reject)")
        except Exception:
            print("set_process_priority bogus -> rejected (correct)")

        c.call("stop"); c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"{e}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
