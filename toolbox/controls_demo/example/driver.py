"""controls_demo example — proof that the shipped demo project actually runs.

Opens toolbox/controls_demo/example, compiles its script, runs it, and asserts the
script reached the plugin's controls-pluginlet def surface through exchange().

This is what keeps the example honest: a demo nothing executes rots into a
broken first impression (this repo has the receipts — a dozen examples were
quarantined for using retired macros). run_qa.py picks this up automatically.

Run:  python toolbox/controls_demo/example/driver.py
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


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_controls_demo_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 10})

        results = []
        end = time.time() + 3.0
        while time.time() < end:
            try:
                ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception:
                break
            if ev.get("name") == "run_result":
                d = ev.get("data", {})
                results.append((d.get("code"), d.get("msg")))
        c.call("stop")

        print(f"run_results in 3s: {len(results)}")
        if results:
            print(f"  first: code={results[0][0]} msg={results[0][1]!r}")
        if not results:
            fails.append("no run_result — the script never ran")
        else:
            # ok(1) means exchange('tick') answered with a def surface carrying
            # the 'ticks' readout — i.e. the controls pluginlet is actually
            # serving the UI tree this example exists to show.
            codes = {code for code, _ in results}
            if 1 not in codes:
                fails.append(f"expected ok(1) from the controls def surface, saw {sorted(codes)}")
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
