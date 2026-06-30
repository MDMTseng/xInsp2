"""qa_recipe_script_instance — regression test for the load_project bug where a
saved recipe's SCRIPT-instance configs were silently dropped on restore.

Background: list_instances / save_project serialize script-declared xi::Instance
objects (name + plugin + full def) into a recipe's "instances" array. But the old
load_project restore loop resolved each instance ONLY through the backend's
xi::InstanceRegistry singleton — and script instances live in the SCRIPT DLL's
OWN registry, unreachable from that singleton. So find() returned null, the def
was discarded, and load_project still reported a clean ok. A vision line would
then run on DEFAULT thresholds while the operator believes the saved recipe
applied: a "fail-reads-as-pass".

This driver:
  1. opens the project + compiles the script (one xi::Instance<Scaler>, factor=5)
  2. tunes the script instance to factor=42 via set_instance_def
  3. captures the recipe via list_instances
  4. RESETS the instance back to its default (reload the script DLL)
  5. writes a recipe project.json = captured scaler + a deliberately-MISSING
     "ghost" instance, then load_project's it
  6. asserts the TUNED factor (42) was re-applied to the script instance
     AND that the load reported an instance_warning for the missing ghost
     (i.e. a partial restore is surfaced, not a silent clean ok)

With the fix reverted, step 6 fails: factor stays 5 (def dropped) and the load
returns a clean ok with no warnings.

Run:  python driver.py
TODO(linux): backend script compile (cl.exe) is Windows-only here; SKIPs on non-nt.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{SUF}"
PORT = 7871
RECIPE = ROOT / "recipe.json"


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout):
            return True
    except OSError:
        return False


def get_factor(c: Client) -> int:
    r = c.call("get_instance_def", {"name": "scaler"})
    if isinstance(r, str):
        r = json.loads(r)
    return int(r.get("factor")) if isinstance(r, dict) and r.get("factor") is not None else -1


def cleanup_artifacts():
    for p in [ROOT / "be.log", RECIPE, ROOT / ".xinsp_owner"]:
        try:
            p.unlink()
        except OSError:
            pass
    # working-copy scratch + any owner markers left under the project
    import shutil
    for d in [ROOT / ".xinsp_work"]:
        shutil.rmtree(d, ignore_errors=True)


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend script compile is Windows-only here")
        return 0
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")
    if port_open(PORT):
        sys.exit(f"FAIL: something already on :{PORT}")

    blog = open(ROOT / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={PORT}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    failures: list[str] = []
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(PORT):
            time.sleep(0.2)
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60) as c:
            c.open_project(str(ROOT), timeout=300)
            c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=180)

            default_factor = get_factor(c)
            print(f"[step] default factor = {default_factor}")
            if default_factor != 5:
                failures.append(f"expected default factor 5, got {default_factor}")

            # Tune the script instance.
            c.set_instance_def("scaler", {"factor": 42})
            if get_factor(c) != 42:
                failures.append("set_instance_def did not apply factor=42")

            # Capture the recipe (script instances come back here).
            li = c.list_instances()
            captured = li.get("instances", []) if isinstance(li, dict) else []
            scaler_entry = next((e for e in captured if e.get("name") == "scaler"), None)
            print(f"[step] captured instances = {json.dumps(captured)}")
            if not scaler_entry or scaler_entry.get("def", {}).get("factor") != 42:
                failures.append(f"list_instances did not capture tuned scaler def: {scaler_entry}")

            # Reset: reload the script DLL so the script instance reverts to default.
            c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=180)
            reset_factor = get_factor(c)
            print(f"[step] after reset, factor = {reset_factor}")
            if reset_factor != 5:
                failures.append(f"reset did not restore default factor 5, got {reset_factor}")

            # Build a recipe = captured scaler + a deliberately-missing instance.
            recipe_instances = list(captured) + [
                {"name": "ghost_missing", "plugin": "Nope", "def": {"factor": 99}},
            ]
            recipe = {
                "name": "qa_recipe_script_instance",
                "script": "inspect.cpp",
                "params": [],
                "instances": recipe_instances,
            }
            RECIPE.write_text(json.dumps(recipe, indent=2), encoding="utf-8")

            # Restore the recipe.
            rsp = c.load_project(str(RECIPE))
            print(f"[step] load_project rsp = {json.dumps(rsp)}")

            # The tuned def must be re-applied to the script instance.
            applied = get_factor(c)
            print(f"[step] after load_project, factor = {applied}")
            if applied != 42:
                failures.append(
                    f"load_project did NOT re-apply tuned factor (got {applied}, "
                    f"expected 42) — script-instance def was dropped (fail-reads-as-pass)")

            # The missing instance must be surfaced as a warning, not a silent ok.
            warns = rsp.get("instance_warnings", []) if isinstance(rsp, dict) else []
            if not any("ghost_missing" in w for w in warns):
                failures.append(
                    f"load_project did not warn about the missing ghost instance "
                    f"(instance_warnings={warns}) — a partial restore reported as clean ok")
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        try:
            be.wait(timeout=5)
        except subprocess.TimeoutExpired:
            be.kill()
        blog.close()
        cleanup_artifacts()

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print("  -", f)
    else:
        print("VERDICT: PASS")
        print("  load_project re-applied the tuned script-instance def (factor=42)")
        print("  and surfaced an instance_warning for the missing instance — no")
        print("  silent fail-reads-as-pass.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
