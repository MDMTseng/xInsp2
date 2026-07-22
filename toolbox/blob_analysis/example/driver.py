"""blob_analysis example — proof that the noise gate is doing the filtering.

The script draws 3 parts (4x4 squares) and 12 specks (single pixels) into one
image and drives blob_analysis twice on it: ungated (min_area=1) and gated
(min_area=10). This driver asserts BOTH halves:

  * ungated the run finds all 15 blobs   — the specks really are in the image;
  * gated it finds exactly the 3 parts   — and their centroids are right.

Only checking the gated 3 would pass just as happily on an image that never had
any noise in it, which would demonstrate nothing.

Run:  python toolbox/blob_analysis/example/driver.py
"""
from __future__ import annotations
import os, re, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402

PORT = int(os.environ.get("PORT", "0")) or free_port()

MSG_RE = re.compile(r"blob raw=(-?\d+) gated=(-?\d+) gate=(-?\d+) geom=(\d)")


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
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_blob_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 10})          # no camera: the script IS the scene

        runs = drain(c, 3.0)
        c.call("stop")
        print(f"run_results: {len(runs)}")
        for code, msg in runs[:2]:
            print(f"   code={code}  {msg}")

        parsed = [(code, MSG_RE.search(msg)) for code, msg in runs]
        parsed = [(code, m) for code, m in parsed if m]
        if len(parsed) < 3:
            fails.append(f"only {len(parsed)} parseable runs — the script barely ran")
        for code, m in parsed:
            raw, gated, gate, geom = (int(m.group(i)) for i in range(1, 5))
            if raw != 15:
                fails.append(f"ungated count was {raw}, want 15 (3 parts + 12 specks) "
                             "— min_area=1 must see the noise")
                break
            if gated != 3:
                fails.append(f"gated count was {gated}, want 3 — min_area={gate} "
                             "did not filter the specks")
                break
            if raw <= gated:
                fails.append(f"gate filtered nothing: raw={raw} gated={gated}")
                break
            if geom != 1:
                fails.append("the `blobs` array did not carry the 3 expected "
                             "areas/centroids — geometry readback broke")
                break
            if code != 1:
                fails.append(f"script verdict was {code}, want ok(1)")
                break

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
