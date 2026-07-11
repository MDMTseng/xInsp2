"""qa_instance_def_recompile — regression test for the bug where a script-declared
xi::Instance def tuned via cmd:set_instance_def was silently LOST on every
hot-recompile (no replay, unlike Params).

Background: compile_and_load replays operator-tuned Param values into the freshly
built script DLL (via g_param_cache) so a recompile doesn't reset them to their
file-scope defaults. Script-declared xi::Instance objects had NO such replay: on
compile_and_load the old DLL is unloaded and the new DLL re-runs its file-scope
xi::Instance ctors, re-seeding every instance with its SOURCE default def. Instance
defs aren't part of xi::state() either, so any instance the operator tuned / taught
/ calibrated via set_instance_def silently reverted to the source default the
moment the script was recompiled — and the project treats this hot-tune loop as
primary. The fix mirrors the param-cache machinery: a g_instance_def_cache populated
on every accepted script-side set_instance_def and replayed after the param replay.

This driver:
  1. opens the project + compiles the script (one xi::Instance<Scaler>, factor=5)
  2. tunes the script instance to factor=42 via set_instance_def
  3. asserts get_instance_def == 42
  4. forces a HOT-RECOMPILE (a second compile_and_load of the SAME file)
  5. asserts get_instance_def is STILL 42 (preserved), NOT the source default 5

With the fix reverted, step 5 fails: factor reverts to 5 (the source default).

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
sys.path.insert(0, str(REPO_ROOT / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = backend_exe()
PORT = free_port()  # ephemeral (was 7873); no squatter cross-talk


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
    for p in [ROOT / "be.log", ROOT / ".xinsp_owner"]:
        try:
            p.unlink()
        except OSError:
            pass
    import shutil
    for d in [ROOT / ".xinsp_work"]:
        shutil.rmtree(d, ignore_errors=True)


def main() -> int:
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
            tuned = get_factor(c)
            print(f"[step] after set_instance_def, factor = {tuned}")
            if tuned != 42:
                failures.append(f"set_instance_def did not apply factor=42, got {tuned}")

            # Force a HOT-RECOMPILE of the SAME file. This unloads the old DLL and
            # re-runs the new DLL's file-scope xi::Instance ctor (re-seeding factor=5).
            c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=180)
            after = get_factor(c)
            print(f"[step] after hot-recompile, factor = {after}")
            if after != 42:
                failures.append(
                    f"hot-recompile did NOT preserve tuned factor (got {after}, "
                    f"expected 42) — script-instance def was lost (no replay)")
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
        print("  compile_and_load replayed the tuned script-instance def (factor=42)")
        print("  across a hot-recompile — no silent revert to the source default.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
