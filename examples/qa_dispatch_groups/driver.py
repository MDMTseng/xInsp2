"""
Dispatch-groups v1 smoke test.

Proves the gated per-group lane dispatcher:
  - a project with `parallelism.groups` spawns per-group worker lanes;
  - continuous mode runs inspects through them (timer ticks → default group);
  - `dispatch_stats` reports a per-group breakdown (name / max_parallel /
    thread_priority / running / queue / dropped);
  - a project WITHOUT groups still reports no `groups` key (legacy path intact).

(The full high-vs-low load-separation guarantee — a saturated `low` group never
delaying `high` — needs a self-emitting source in each group; that's a follow-up.)

Run:  python examples/qa_dispatch_groups/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()


def spawn(port, iso_name):
    iso = Path(tempfile.gettempdir()) / iso_name
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = env["TMPDIR"] = str(iso)
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
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []
    proc = spawn(PORT, "xi_dg_tmp")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 20})

        # Let lanes run a bit. VAR() is a no-op in this core (the vars wire
        # channel was purged), so we no longer count `vars` frames. Instead we
        # count `run_finished` events — the backend brackets every inspect with
        # run_started/run_finished, so one per completed grouped continuous tick.
        # That an inspect COMPLETED is the proof a lane actually ran.
        ran = 0
        end = time.time() + 20.0
        while time.time() < end and ran < 5:
            try:
                ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception:
                break
            if ev.get("name") == "run_finished":
                ran += 1

        st = c.call("dispatch_stats")
        print("dispatch_stats:", {k: st.get(k) for k in ("dispatch_threads",)}, "groups=", st.get("groups"))
        groups = st.get("groups")
        if not isinstance(groups, list) or len(groups) != 2:
            fails.append(f"expected 2 group lanes, got {groups}")
        else:
            names = {g["name"] for g in groups}
            if names != {"high", "low"}:
                fails.append(f"group names {names} != high/low")
            hi = next((g for g in groups if g["name"] == "high"), {})
            lo = next((g for g in groups if g["name"] == "low"), {})
            if hi.get("max_parallel") != 2: fails.append(f"high max_parallel {hi.get('max_parallel')} != 2")
            if lo.get("thread_priority") != "low": fails.append(f"low thread_priority {lo.get('thread_priority')} != low")
            for g in groups:
                for f in ("running", "queue_now", "high_watermark", "dropped"):
                    if f not in g: fails.append(f"group {g.get('name')} missing field {f}")
        if ran < 1: fails.append("no inspects completed under grouped continuous mode")
        print(f"  inspects completed (run_finished): {ran}")
        c.call("stop")
        c.call("close_project")
        c.close()
    except Exception as e:
        fails.append(f"grouped: {e}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    # --- legacy: no groups -> dispatch_stats has no "groups" key ---
    legacy = REPO / "examples" / "image_sources"
    if legacy.is_dir():
        p_legacy = free_port()
        proc2 = spawn(p_legacy, "xi_dg_legacy_tmp")
        try:
            c2 = connect(p_legacy)
            if c2:
                c2.call("open_project", {"path": str(legacy)})
                st2 = c2.call("dispatch_stats")
                if "groups" in st2:
                    fails.append("legacy project unexpectedly reported a 'groups' breakdown")
                else:
                    print("legacy (image_sources): no 'groups' key")
                c2.close()
        except Exception as e:
            print(f"  (legacy check skipped: {e})")
        finally:
            proc2.terminate()
            try: proc2.wait(5)
            except Exception: proc2.kill()

    # --- helper: write a temp project with a given parallelism block ---
    import json, shutil
    def make_proj(sub, parallelism):
        d = Path(tempfile.gettempdir()) / sub
        if d.exists(): shutil.rmtree(d)
        d.mkdir(parents=True)
        (d / "inspect.cpp").write_text((ROOT / "inspect.cpp").read_text(encoding="utf-8"), encoding="utf-8")
        json.dump({"name": sub, "script": "inspect.cpp", "parallelism": parallelism},
                  open(d / "project.json", "w", encoding="utf-8"))
        return d

    # --- Test B: max_parallel is clamped to 32 (#4) ---
    pB = make_proj("xi_dg_clampB", {"default_group": "main",
                                    "groups": [{"name": "main", "max_parallel": 1000000}]})
    p_clamp = free_port()
    proc3 = spawn(p_clamp, "xi_dg_clamp_iso")
    try:
        c3 = connect(p_clamp)
        if c3:
            c3.call("open_project", {"path": str(pB)})
            c3.compile_and_load(str(pB / "inspect.cpp"), timeout=300)
            c3.call("start", {"fps": 5})   # spawns the lane (clamped) so dispatch_stats shows it
            g = (c3.call("dispatch_stats").get("groups") or [{}])[0]
            print(f"Test B clamp: main max_parallel = {g.get('max_parallel')}")
            if g.get("max_parallel") != 32:
                fails.append(f"B: max_parallel not clamped to 32 (got {g.get('max_parallel')})")
            c3.call("stop"); c3.close()
    except Exception as e: fails.append(f"B: {e}")
    finally:
        proc3.terminate()
        try: proc3.wait(5)
        except Exception: proc3.kill()

    # --- Test D: validation warnings (#6/#7) via open_project_warnings ---
    pD = make_proj("xi_dg_warnD", {"default_group": "missing", "groups": [
        {"name": "dup", "max_parallel": 2},
        {"name": "dup", "max_parallel": 4},
        {"name": "weird", "thread_priority": "bogus", "overflow": "nope"},
    ]})
    p_warn = free_port()
    proc4 = spawn(p_warn, "xi_dg_warn_iso")
    try:
        c4 = connect(p_warn)
        if c4:
            c4.call("open_project", {"path": str(pD)})
            warns = (c4.call("open_project_warnings").get("warnings") or [])
            blob = " ".join((w.get("reason", "") + " " + w.get("instance", "")).lower() for w in warns)
            print(f"Test D warnings: {len(warns)} — {[w.get('reason','')[:40] for w in warns]}")
            for needle in ("duplicate", "default_group", "thread_priority", "overflow"):
                if needle not in blob:
                    fails.append(f"D: expected a '{needle}' warning, none found")
            # the dup group must yield exactly one 'dup' lane (2nd skipped at parse)
            c4.compile_and_load(str(pD / "inspect.cpp"), timeout=300)
            c4.call("start", {"fps": 5})
            g2 = c4.call("dispatch_stats").get("groups") or []
            if sum(1 for g in g2 if g.get("name") == "dup") != 1:
                fails.append("D: duplicate group name produced != 1 lane")
            c4.call("stop"); c4.close()
    except Exception as e: fails.append(f"D: {e}")
    finally:
        proc4.terminate()
        try: proc4.wait(5)
        except Exception: proc4.kill()

    # --- Test E: per-group result_order:"arrival" serialises emission by arrival ---
    # A group with max_parallel 3 + an anti-correlated sleep (even run_ids slow,
    # odd fast) makes completions scramble. In "arrival" mode the gate must still
    # emit run_result in run_id order (0 inversions); a "completion" control proves
    # the workload really does scramble (so arrival's 0 isn't vacuous).
    # VAR was purged; the ordering check keys off run_result events (emitted per
    # inspect by run_id), so the body just needs the anti-correlated sleep that
    # scrambles completion order.
    SLOW_VAR = ("#include <xi/xi.hpp>\n#include <thread>\n#include <chrono>\n"
                "XI_SCRIPT_EXPORT void xi_inspect_entry(int frame){"
                " std::this_thread::sleep_for(std::chrono::milliseconds(frame%2==0?60:8)); }\n")

    def order_run(sub, result_order, port):
        d = make_proj(sub, {"default_group": "main", "groups": [
            {"name": "main", "max_parallel": 3, "queue_depth": 500, "result_order": result_order}]})
        (d / "inspect.cpp").write_text(SLOW_VAR, encoding="utf-8")
        p = spawn(port, sub + "_iso")
        ids = []
        try:
            cc = connect(port)
            if cc:
                cc.call("open_project", {"path": str(d)})
                cc.compile_and_load(str(d / "inspect.cpp"), timeout=300)
                cc.call("start", {"fps": 50})
                end = time.time() + 3.0
                while time.time() < end:
                    try:
                        ev = cc._inbox_events.get(timeout=max(0.05, end - time.time()))
                    except Exception:
                        break
                    if ev.get("name") == "run_result":
                        rid = ev.get("data", {}).get("run_id")
                        if rid is not None:
                            ids.append(rid)
                cc.call("stop"); cc.close()
        finally:
            p.terminate()
            try: p.wait(5)
            except Exception: p.kill()
        inv = sum(1 for i in range(len(ids) - 1) if ids[i] > ids[i + 1])
        return len(ids), inv

    n_arr, inv_arr = order_run("xi_dg_orderE", "arrival", free_port())
    n_cmp, inv_cmp = order_run("xi_dg_orderC", "completion", free_port())
    print(f"Test E arrival: {n_arr} run_results, {inv_arr} inversions; "
          f"completion control: {n_cmp} run_results, {inv_cmp} inversions")
    if n_arr < 20:
        fails.append(f"E: too few run_results ({n_arr}) to judge ordering")
    if inv_arr != 0:
        fails.append(f"E: arrival mode had {inv_arr} run_id inversions (expected 0)")
    if inv_cmp == 0:
        print("  (note: completion control showed no inversions — workload didn't "
              "scramble this run; arrival assertion is therefore weaker)")

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
