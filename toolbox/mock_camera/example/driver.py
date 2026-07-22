"""mock_camera example — proof that the auto-exposure loop actually closes.

The camera starts at gain 0.2, far below the target band, so the first frames
MUST be out of band. If the script's correction reaches the camera, later frames
must land in band and stay there. Asserting both halves is the point: a demo
that merely ends in band would also pass if the camera had started there.

Run:  python toolbox/mock_camera/example/driver.py
"""
from __future__ import annotations
import os, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402

PORT = int(os.environ.get("PORT", "0")) or free_port()


def drain(c, seconds: float) -> list:
    """Collect (code, msg) from run_result events for `seconds`."""
    out = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
        except Exception:
            break
        if ev.get("name") == "run_result":
            d = ev.get("data", {})
            out.append((d.get("code"), d.get("msg", "")))
    return out


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_mockcam_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 0})           # trigger-only: the camera drives
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "start"}})

        runs = drain(c, 6.0)
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "stop"}})
        print(f"run_results: {len(runs)}")
        for code, msg in runs[:4]:
            print(f"   early  code={code}  {msg}")
        for code, msg in runs[-3:]:
            print(f"   late   code={code}  {msg}")

        if len(runs) < 8:
            fails.append(f"only {len(runs)} runs — too few to show convergence")
        else:
            # code 0 = still settling, 1 = in band, -1 = the door rejected us.
            if any(code == -1 for code, _ in runs):
                fails.append("camera door rejected a set_gain command")
            if runs[0][0] != 0:
                fails.append(f"first run was already in band (code={runs[0][0]}) "
                             "— the loop was never actually exercised")
            late = [code for code, _ in runs[-3:]]
            if not all(code == 1 for code in late):
                fails.append(f"last 3 runs not converged: codes={late}")

        c.call("stop")
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
