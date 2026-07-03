"""
local_image_source AUTO mode — self-emit a local folder of images.

Reuses the "local" plugin with the new auto_ms config: a worker re-scans the folder
and emits the next image every auto_ms (cycling). The script reads each image and
emits a brightness verdict. Started TRIGGER-ONLY (no timer) so the source is the
sole driver. Asserts: continuous auto-emit produced several run_results from `cam`
(i.e. the auto worker is ticking + routing), with real image frames.

Run:  python examples/qa_local_auto/driver.py   (Windows; backend built)
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
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_local_auto_iso"
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
        c.call("start", {"fps": 0})   # trigger-only: the auto worker is the driver

        results = []          # (code, msg) tuples from cam
        end = time.time() + 3.0
        while time.time() < end:
            try: ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception: break
            if ev.get("name") == "run_result":
                d = ev.get("data", {})
                results.append((d.get("code"), d.get("msg")))
        c.call("stop")

        n_ok = sum(1 for code, _ in results if code == 1)
        print(f"auto-emitted run_results in 3s: {len(results)} (ok={n_ok})")
        # auto_ms=300 -> ~10 emits in 3s. Allow slack for compile/startup.
        if len(results) < 4:
            fails.append(f"expected several auto run_results, got {len(results)}")
        if n_ok < 1:
            fails.append("no successful brightness verdict (image not delivered?)")
        c.call("close_project"); c.close()
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
