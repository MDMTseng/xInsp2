"""
BUG #19 e2e — a single-image emit keyed by "frame" must reach a script reading
image("frame") from the LIVE source path (not just via cmd:run), AND the frame
that script re-emits must arrive at the consumer keyed "frame".

The "cam" source (local_image_source, auto mode) self-emits a single image as
Record().image("frame", img). The inspect script reads it two ways and surfaces
the result through the `expose` plugin on channel "qa" (VAR/EMIT are no-ops in
this core; expose is the official data-out surface):
  values: fw = width of t.image("frame")           (documented contract)
          nw = width of t.image(<instance name>)    (legacy read; sole-image fallback)
  image:  the frame read via image("frame"), re-emitted keyed "frame"

Asserts:
  1. LIVE source path: an XEX1 frame on channel "qa" carries an image keyed
     "frame" (the bug: nothing keyed "frame" reached the consumer before the fix).
  2. LIVE source path: fw>0 (image("frame") resolved) AND nw>0 (instance-name
     fallback resolved) in that frame's values.
  3. cmd:run inject path: the SAME script run via cmd:run --frame surfaces a "qa"
     frame with fw>0 and an image keyed "frame".

Run:  python examples/qa_emit_frame_key/driver.py
"""
from __future__ import annotations
import os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(ROOT.parents[0] / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402
from xex1 import collect_frames, pull_latest, subscribe  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "0")) or free_port()
FRAME = REPO / "examples" / "blob_tracker" / "frames" / "frame_00.png"


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
        subscribe(c, ["qa"])          # gate the expose channel ON so frames push

        # ---- 1+2. LIVE source path: the auto worker self-emits image("frame"). ----
        c.call("start", {"fps": 0})   # trigger-only: the source is the sole driver
        c.drain_binary()              # zero the binary baseline
        c.exchange_instance("cam", {"command": "auto_on", "value": 200})  # force self-emit
        live = None
        n_frames = 0
        end = time.time() + 6.0
        while time.time() < end:
            for fr in collect_frames(c):
                if fr.get("channel") != "qa":
                    continue
                n_frames += 1
                if (fr.get("values") or {}).get("fw"):
                    live = fr      # keep the latest frame that actually read a width
            if live is not None:
                break
            time.sleep(0.1)
        c.exchange_instance("cam", {"command": "auto_off"})
        c.call("stop")
        lv = (live or {}).get("values", {})
        live_imgs = (live or {}).get("images", {})
        live_fw, live_nw = lv.get("fw"), lv.get("nw")
        print(f"LIVE: frames={n_frames} fw={live_fw} nw={live_nw} image_keys={list(live_imgs)}")
        if live is None:
            fails.append("live: no XEX1 frame on channel 'qa' arrived from the source path")
        else:
            if "frame" not in live_imgs:
                fails.append(f"live: re-emitted image not keyed 'frame' (keys={list(live_imgs)})")
            if not live_fw or live_fw <= 0:
                fails.append(f"live image(\"frame\") returned null (fw={live_fw})")
            if not live_nw or live_nw <= 0:
                fails.append(f"live instance-name fallback returned null (nw={live_nw})")

        # ---- 3. cmd:run inject path: same script, frame injected under "frame". ----
        time.sleep(0.3)               # let any trailing auto-emit settle
        c.drain_binary()              # clear stream so pull/collect see the run's frame
        c.run(frame_path=str(FRAME))
        # pull_latest is race-free (exchange `get` builds the frame on demand from
        # the channel's stored record), so it reliably returns THIS run's output.
        run = None
        end = time.time() + 10.0
        while time.time() < end and run is None:
            run = pull_latest(c, "qa")
            if run is None or not (run.get("values") or {}).get("fw"):
                run = None
                time.sleep(0.1)
        rv = (run or {}).get("values", {})
        run_imgs = (run or {}).get("images", {})
        run_fw = rv.get("fw")
        print(f"CMD:RUN: fw={run_fw} nw={rv.get('nw')} image_keys={list(run_imgs)}")
        if run is None:
            fails.append("cmd:run: no 'qa' frame surfaced for the injected frame")
        else:
            if not run_fw or run_fw <= 0:
                fails.append(f"cmd:run image(\"frame\") returned null (fw={run_fw})")
            if "frame" not in run_imgs:
                fails.append(f"cmd:run: re-emitted image not keyed 'frame' (keys={list(run_imgs)})")

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
