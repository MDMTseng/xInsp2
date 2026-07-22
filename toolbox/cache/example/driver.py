"""cache example — proof that the shipped demo project actually runs.

Runs the camera briefly, STOPS it, then sends the buffer a `replay_last` and
asserts a further run_result arrives. With no camera running, that run can only
have come from the ring — which is the whole claim this example makes.

Run:  python toolbox/cache/example/driver.py
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
    """Collect run_result events for `seconds`."""
    out = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
        except Exception:
            break
        if ev.get("name") == "run_result":
            out.append((ev.get("data", {}).get("code"), ev.get("data", {}).get("msg")))
    return out


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_cache_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 0})           # trigger-only: the camera drives
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "start"}})

        live = drain(c, 2.5)
        print(f"live run_results: {len(live)}")
        if not live:
            fails.append("camera produced no runs — nothing was buffered")

        # Stop the camera. From here on, ONLY a replay can produce a run.
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "stop"}})
        drain(c, 1.0)                          # let in-flight frames settle

        c.call("exchange_instance", {"name": "buffer", "cmd": {"command": "replay_last"}})
        replayed = drain(c, 2.0)
        print(f"run_results after replay_last (camera stopped): {len(replayed)}")
        if not replayed:
            fails.append("replay_last produced no run — the ring did not re-emit")

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
