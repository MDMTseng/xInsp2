"""
BUG #19 e2e — a single-image emit keyed by "frame" must reach a script reading
image("frame") from the LIVE source path (not just via cmd:run).

The "cam" source (local_image_source, auto mode) self-emits a single image as
Record().image("frame", img). The inspect script reports, via xi::result()
(run_result event — VAR/EMIT are no-ops in this core), the width it read via
  fw : t.image("frame")              (documented contract)
  nw : t.image(<instance name>)      (legacy read, reader-side sole-image fallback)
Verdict: ok code 1 with msg "fw=W nw=W" when BOTH reads got the frame.

Asserts:
  1. LIVE source path: a code-1 run_result with fw>0 (the bug: fw was 0 before the fix).
  2. LIVE source path: nw>0 in that same result (instance-name fallback resolves the frame).
  3. cmd:run inject path: the SAME script run via cmd:run --frame yields a code-1
     run_result with fw>0.

Run:  python examples/qa_emit_frame_key/driver.py
"""
from __future__ import annotations
import os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7877"))
FRAME = REPO / "examples" / "blob_tracker" / "frames" / "frame_00.png"

_WH = re.compile(r"fw=(-?\d+)\s+nw=(-?\d+)")


def parse_wh(msg: str):
    m = _WH.search(msg or "")
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_emit_frame_key_iso"
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

        # ---- 1+2. LIVE source path: the auto worker self-emits image("frame"). ----
        c.call("start", {"fps": 0})   # trigger-only: the source is the sole driver
        c.exchange_instance("cam", {"command": "auto_on", "value": 200})  # force self-emit
        live_fw = live_nw = None
        n_results = 0
        end = time.time() + 6.0
        while time.time() < end and live_fw is None:
            try:
                ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception:
                break
            if ev.get("name") != "run_result":
                continue
            d = ev.get("data", {})
            if d.get("code") in (None, 0):   # skip drops / NA
                continue
            n_results += 1
            fw, nw = parse_wh(d.get("msg", ""))
            if fw is not None:
                live_fw, live_nw = fw, nw
        c.call("stop")
        print(f"LIVE: results={n_results} fw={live_fw} nw={live_nw}")
        if not live_fw or live_fw <= 0:
            fails.append(f"live image(\"frame\") returned null (fw={live_fw})")
        if not live_nw or live_nw <= 0:
            fails.append(f"live instance-name fallback returned null (nw={live_nw})")

        # ---- 3. cmd:run inject path: same script, frame injected under "frame". ----
        # (Use the raw command, not Client.run(): VAR/EMIT are no-ops in this core
        # so no `vars` wire message is sent, and Client.run() would block on it.
        # The run_result EVENT carries our verdict, same as the live path.)
        c.exchange_instance("cam", {"command": "auto_off"})  # quiesce the source
        c._drain(c._inbox_events)
        rsp = c.call("run", {"frame_path": str(FRAME)})
        run_id = rsp.get("run_id")
        run_fw = run_nw = None
        end = time.time() + 10.0
        while time.time() < end and run_fw is None:
            try:
                ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception:
                break
            if ev.get("name") != "run_result":
                continue
            d = ev.get("data", {})
            if d.get("code") in (None, 0):
                continue
            run_fw, run_nw = parse_wh(d.get("msg", ""))
        print(f"CMD:RUN: run_id={run_id} fw={run_fw} nw={run_nw}")
        if not run_fw or run_fw <= 0:
            fails.append(f"cmd:run image(\"frame\") returned null (fw={run_fw})")

        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"{type(e).__name__}: {e!r}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
