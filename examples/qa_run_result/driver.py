"""
Per-run Result v1 smoke test.

Proves the `run_result` event:
  Test 1 (verdict) — drive cmd:run 4x; the cycling script emits NA / ok1 / ng2 /
    (a reserved system code that the API must clamp to -1). Assert the run_result
    event's `code` for each run_id is exactly [0, 1, -2, -1].
  Test 2 (dropped) — a slow inspect with queue_depth:1 + drop_oldest under a fast
    timer; assert at least one run_result arrives with code == XI_SYS_DROPPED
    (-999001), i.e. the framework fills the non-run case so the stream has no gap.

Run:  python examples/qa_run_result/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7896"))
XI_SYS_DROPPED = -999001


def spawn(port, iso_name):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / iso_name
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


def wait_run_result(c, run_id, timeout=15.0):
    """Pull events until the run_result for `run_id` shows up (ignoring others)."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
        except Exception:
            break
        if ev.get("name") == "run_result" and ev.get("data", {}).get("run_id") == run_id:
            return ev["data"]
    return None


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []

    # --- Test 1: verdict codes via cmd:run ---
    proc = spawn(PORT, "xi_rr_verdict")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        codes = []
        for _ in range(4):
            data = c.call("run", {})
            rid = data["run_id"]
            rr = wait_run_result(c, rid)
            codes.append(rr.get("code") if rr else None)
        print(f"Test 1 verdict codes: {codes}  (expect [0, 1, -2, -1])")
        if codes != [0, 1, -2, -1]:
            fails.append(f"1: run_result codes {codes} != [0, 1, -2, -1]")
        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"1: {e}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    # --- Test 2: dropped frame -> XI_SYS_DROPPED ---
    import json, shutil
    sd = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_rr_slow"
    if sd.exists(): shutil.rmtree(sd)
    sd.mkdir(parents=True)
    (sd / "inspect.cpp").write_text(
        "#include <xi/xi.hpp>\n#include <thread>\n#include <chrono>\n"
        "XI_SCRIPT_EXPORT void xi_inspect_entry(int frame){ VAR(frame_n,frame);"
        " std::this_thread::sleep_for(std::chrono::milliseconds(120)); }\n",
        encoding="utf-8")
    json.dump({"name": "slow", "script": "inspect.cpp",
               "parallelism": {"dispatch_threads": 1, "queue_depth": 1, "overflow": "drop_oldest"}},
              open(sd / "project.json", "w", encoding="utf-8"))
    proc2 = spawn(PORT + 1, "xi_rr_slow_iso")
    try:
        c2 = connect(PORT + 1)
        if c2:
            c2.call("open_project", {"path": str(sd)})
            c2.compile_and_load(str(sd / "inspect.cpp"), timeout=300)
            c2.call("start", {"fps": 50})   # 20ms ticks vs 120ms inspect, depth 1 -> drops
            time.sleep(2.5)
            c2.call("stop")
            dropped = 0
            while True:
                try:
                    ev = c2._inbox_events.get_nowait()
                except Exception:
                    break
                if ev.get("name") == "run_result" and ev.get("data", {}).get("code") == XI_SYS_DROPPED:
                    dropped += 1
            print(f"Test 2 dropped run_results: {dropped}")
            if dropped < 1:
                fails.append("2: expected >=1 XI_SYS_DROPPED run_result, got 0")
            c2.close()
    except Exception as e:
        fails.append(f"2: {e}")
    finally:
        proc2.terminate()
        try: proc2.wait(5)
        except Exception: proc2.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
