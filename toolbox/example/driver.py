"""toolbox integration example — proof that the whole station composes.

Five plugins, and an assertion that each one actually did its job:

  cam + det   runs arrive carrying a blob_count-based verdict
  view        expose lists the "station" channel and has images on it
  saver       forcing NG (max_blobs -> 0) makes capture files appear on disk,
              and NOT before — so the writer is driven by the verdict, not by
              every frame
  ring        with the camera STOPPED, a replay still produces a run, so the
              retention path survives inside the composed graph

Run:  python toolbox/example/driver.py
"""
from __future__ import annotations
import os, shutil, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402

PORT = int(os.environ.get("PORT", "0")) or free_port()
CAPTURES = ROOT / "instances" / "saver" / "captures"


def drain(c, seconds: float) -> list:
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


def n_captures() -> int:
    return len(list(CAPTURES.glob("*.xex1"))) if CAPTURES.is_dir() else 0


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    shutil.rmtree(CAPTURES, ignore_errors=True)      # start from a clean disk

    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_toolbox_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 0})
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "start"}})

        # --- phase 1: the line runs clean --------------------------------
        good = drain(c, 3.0)
        print(f"[1] runs with the generous limit: {len(good)}  e.g. {good[:1]}")
        if len(good) < 5:
            fails.append(f"only {len(good)} runs — the chain is not turning")
        elif not all(code == 1 for code, _ in good):
            fails.append(f"expected all PASS at max_blobs=64, got {set(c for c,_ in good)}")

        before = n_captures()
        if before != 0:
            fails.append(f"{before} captures written while everything PASSED — "
                         "the saver is not gated on the verdict")

        # --- phase 2: force NG, the saver must start writing --------------
        c.set_param("max_blobs", 0)
        bad = drain(c, 3.0)
        after = n_captures()
        print(f"[2] runs after max_blobs=0: {len(bad)}  captures on disk: {after}")
        if bad and not any(code == -1 for code, _ in bad):
            fails.append("no NG verdict after max_blobs=0")
        if after == 0:
            fails.append("NG frames produced no .xex1 captures — record_save "
                         "never ran, or wrote somewhere else")

        # --- phase 3: expose is holding the channel ------------------------
        ch = c.call("exchange_instance",
                    {"name": "view", "cmd": {"command": "list_channels"}})
        blob = str(ch)
        print(f"[3] expose list_channels: {blob[:160]}")
        if "station" not in blob:
            fails.append("expose has no 'station' channel — nothing was pushed")

        # --- phase 4: retention survives inside the composed graph ---------
        c.call("exchange_instance", {"name": "cam", "cmd": {"command": "stop"}})
        drain(c, 1.0)                                   # let in-flight settle
        c.call("exchange_instance", {"name": "ring", "cmd": {"command": "replay_last"}})
        replayed = drain(c, 2.0)
        print(f"[4] runs after replay_last with the camera stopped: {len(replayed)}")
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
        shutil.rmtree(CAPTURES, ignore_errors=True)  # captures are output, not fixture

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
