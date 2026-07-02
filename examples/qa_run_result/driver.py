"""
Per-run Result / run-outcome contract test.

Locks in the identity + outcome contract now carried by the `run_result` event
(schema "xi.run-outcome/1"). Reviews' "truth invariants" want a TEST, not prose:
this driver FAILS (non-zero exit) if any contract field is missing or a
class/code mapping is violated.

  Test 1 (outcome classes + identity) — drive cmd:run 6x; the cycling script
    (inspect.cpp) emits one run per outcome CLASS the script can drive:
        run_id r+0  no verdict            -> code -999005 class "no_verdict"
        run_id r+1  xi::ok(1)             -> code +1      class "ok"
        run_id r+2  xi::ng(2)             -> code -2      class "ng"
        run_id r+3  xi::result(0)         -> code  0      class "na"
        run_id r+4  throw (caught crash)  -> code -999002 class "crashed"
        run_id r+5  xi::result(-999001)   -> code  0      class "na"  (host refuses
                                             the reserved system band -> NA + warn)
    For every run we assert:
      - code == expected, class == expected (the code->class derivation contract)
      - schema == "xi.run-outcome/1"
      - boot_id present, non-empty, and IDENTICAL across all runs in this process
      - run_id strictly increases run to run
      - inspection_id == f"{station_id}/{boot_id}/{run_id}"  (station_id may be "")
      - script_generation present and > 0 (a script is loaded)
      - trigger_id present + 32-char lowercase hex on the runs we inject `meta`
        into (a plain cmd:run has no trigger, so trigger_id is legitimately
        omitted there — asserted absent on the no-meta runs).

  Test 2 (dropped) — a slow inspect with queue_depth:1 + drop_oldest under a fast
    timer; assert at least one run_result arrives with code == XI_SYS_DROPPED
    (-999001) class "dropped", i.e. the framework fills the non-run case so the
    stream has no gap.

The "crashed" case is driven for real: the script throws on its frame and the
host's exception/SEH guard catches it (the process stays up), emitting
XI_SYS_CRASHED. The "dropped" case is driven by Test 2 (it cannot be produced
from inside a single inspect body).

Run:  python examples/qa_run_result/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7896"))
SCHEMA = "xi.run-outcome/1"
XI_SYS_DROPPED    = -999001
XI_SYS_CRASHED    = -999002
XI_SYS_NO_VERDICT = -999005
HEX32 = re.compile(r"^[0-9a-f]{32}$")


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

    # --- Test 1: outcome classes + identity contract via cmd:run ---
    # Each entry: (expected code, expected class, inject `meta`? -> trigger_id present)
    EXPECT = [
        (XI_SYS_NO_VERDICT, "no_verdict", False),  # case 0: set nothing
        (1,                 "ok",         True),   # case 1: xi::ok(1)
        (-2,                "ng",         True),   # case 2: xi::ng(2)
        (0,                 "na",         True),   # case 3: xi::result(0) explicit NA
        (XI_SYS_CRASHED,    "crashed",    True),   # case 4: throw -> caught crash
        (0,                 "na",         False),  # case 5: reserved-band squat -> NA+warn
    ]
    proc = spawn(PORT, "xi_rr_verdict")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)

        rows = []  # (run_id, run_result data, want_code, want_class, want_trigger)
        for want_code, want_class, want_trig in EXPECT:
            args = {"meta": {"probe": want_class}} if want_trig else {}
            data = c.call("run", args)
            rid = data["run_id"]
            rr = wait_run_result(c, rid) or {}
            rows.append((rid, rr, want_code, want_class, want_trig))

        codes = [rr.get("code") for _, rr, _, _, _ in rows]
        classes = [rr.get("class") for _, rr, _, _, _ in rows]
        print(f"Test 1 codes:   {codes}")
        print(f"Test 1 classes: {classes}")

        boot_ids = set()
        prev_run_id = None
        for rid, rr, want_code, want_class, want_trig in rows:
            tag = f"run_id={rid} class={want_class!r}"
            if not rr:
                fails.append(f"1[{tag}]: no run_result event arrived"); continue

            # --- outcome: code + code->class derivation ---
            if rr.get("code") != want_code:
                fails.append(f"1[{tag}]: code {rr.get('code')} != {want_code}")
            if rr.get("class") != want_class:
                fails.append(f"1[{tag}]: class {rr.get('class')!r} != {want_class!r}")

            # --- schema tag ---
            if rr.get("schema") != SCHEMA:
                fails.append(f"1[{tag}]: schema {rr.get('schema')!r} != {SCHEMA!r}")

            # --- msg present (string) ---
            if not isinstance(rr.get("msg"), str):
                fails.append(f"1[{tag}]: msg missing/not a string: {rr.get('msg')!r}")

            # --- boot_id: present, non-empty, stable across the process ---
            bid = rr.get("boot_id")
            if not bid or not isinstance(bid, str):
                fails.append(f"1[{tag}]: boot_id missing/empty: {bid!r}")
            else:
                boot_ids.add(bid)

            # --- run_id strictly increasing ---
            if prev_run_id is not None and not (rid > prev_run_id):
                fails.append(f"1[{tag}]: run_id {rid} did not increase past {prev_run_id}")
            prev_run_id = rid

            # --- inspection_id == "<station_id>/<boot_id>/<run_id>" ---
            station = rr.get("station_id", "")   # additive; omitted (absent) when empty
            insp = rr.get("inspection_id")
            want_insp = f"{station}/{bid}/{rid}"
            if insp != want_insp:
                fails.append(f"1[{tag}]: inspection_id {insp!r} != {want_insp!r}")

            # --- script_generation present + > 0 (a script IS loaded) ---
            gen = rr.get("script_generation")
            if not isinstance(gen, int) or gen <= 0:
                fails.append(f"1[{tag}]: script_generation missing/<=0: {gen!r}")

            # --- trigger_id: 32-char lowercase hex when a trigger exists (meta run);
            #     legitimately ABSENT on a plain cmd:run (no trigger). ---
            tid = rr.get("trigger_id")
            if want_trig:
                if not (isinstance(tid, str) and HEX32.match(tid)):
                    fails.append(f"1[{tag}]: trigger_id {tid!r} not 32-char lowercase hex")
            else:
                if tid:
                    fails.append(f"1[{tag}]: trigger_id should be absent on a no-meta run, got {tid!r}")

        # boot_id must be identical for every run in this one process.
        if len(boot_ids) > 1:
            fails.append(f"1: boot_id not stable within process: {sorted(boot_ids)}")

        # The reserved-band squat run must name the offending code + call it reserved.
        squat_msg = rows[5][1].get("msg", "")
        if "-999001" not in squat_msg or "reserved" not in squat_msg:
            fails.append(f"1: reserved-code run msg should name the bad code, got {squat_msg!r}")

        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"1: {e}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    # --- Test 2: dropped frame -> XI_SYS_DROPPED / class "dropped" ---
    import json, shutil
    sd = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_rr_slow"
    if sd.exists(): shutil.rmtree(sd)
    sd.mkdir(parents=True)
    # A deliberately slow inspect so the 20ms timer outruns it and the depth-1
    # queue overflows (drop_oldest) -> XI_SYS_DROPPED. Only the sleep matters here;
    # it sets no result (the drop marker is synthesized by the framework, not the
    # script). NOTE: the removed v9 `VAR(...)` macro is intentionally NOT used —
    # it no longer exists in the framework and would fail to compile.
    (sd / "inspect.cpp").write_text(
        "#include <xi/xi.hpp>\n#include <thread>\n#include <chrono>\n"
        "XI_SCRIPT_EXPORT void xi_inspect_entry(int /*frame*/){"
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
            dropped_class_ok = True
            while True:
                try:
                    ev = c2._inbox_events.get_nowait()
                except Exception:
                    break
                d = ev.get("data", {})
                if ev.get("name") == "run_result" and d.get("code") == XI_SYS_DROPPED:
                    dropped += 1
                    # The drop path must also carry the schema tag + "dropped" class.
                    if d.get("class") != "dropped":
                        dropped_class_ok = False
                        fails.append(f"2: dropped run_result class {d.get('class')!r} != 'dropped'")
                    if d.get("schema") != SCHEMA:
                        fails.append(f"2: dropped run_result schema {d.get('schema')!r} != {SCHEMA!r}")
            print(f"Test 2 dropped run_results: {dropped} (class ok={dropped_class_ok})")
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
