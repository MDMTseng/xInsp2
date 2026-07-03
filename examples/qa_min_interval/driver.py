"""
Per-group min_interval_ms rate limit.

Both groups receive a steady 20/s. "limited" has min_interval_ms:100 (so dispatch
starts are >=100ms apart -> ~10/s, with the surplus coalesced away via
queue_depth:5 drop_oldest); "free" is uncapped. The inspect is trivial (5ms) so the
cap, not compute, is the limit. Asserts:
  - dispatch_stats reports limited.min_interval_ms == 100;
  - limited group's processed rate is ~10/s (capped, well below its 20/s input)
    and clearly lower than free's;
  - limited group dropped surplus events (queue stayed bounded).

Run:  python examples/qa_min_interval/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, subprocess, sys, time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "0")) or free_port()
WINDOW = 4.0


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_mi_iso"
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
        c.call("start", {"fps": 0})

        n = defaultdict(int)
        t0 = time.time(); end = t0 + WINDOW
        while time.time() < end:
            try: ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception: break
            if ev.get("name") == "run_result" and ev.get("data", {}).get("code") == 1:
                n[ev["data"].get("group")] += 1
        elapsed = time.time() - t0

        st = {g["name"]: g for g in (c.call("dispatch_stats").get("groups") or [])}
        c.call("stop"); c.call("close_project"); c.close()

        mi = st.get("limited", {}).get("min_interval_ms")
        lim_rate = n["limited"] / elapsed
        free_rate = n["free"] / elapsed
        dropped = st.get("limited", {}).get("dropped", 0)
        print(f"dispatch_stats limited.min_interval_ms = {mi}")
        print(f"limited: {n['limited']} results -> {lim_rate:.1f}/s  (cap ~10/s),  dropped {dropped}")
        print(f"free:    {n['free']} results -> {free_rate:.1f}/s")

        if mi != 100:
            fails.append(f"limited min_interval_ms reported {mi}, expected 100")
        if not (6.0 <= lim_rate <= 14.0):
            fails.append(f"limited rate {lim_rate:.1f}/s not ~10/s (cap not enforced?)")
        if free_rate <= lim_rate + 2:
            fails.append(f"free ({free_rate:.1f}/s) not clearly above limited ({lim_rate:.1f}/s)")
        if dropped < 1:
            fails.append("limited group did not drop surplus (rate cap not back-pressuring?)")
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
