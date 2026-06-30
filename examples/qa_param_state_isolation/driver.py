"""qa_param_state_isolation — cross-project param-leak regression (Bug #8).

The backend caches every accepted cmd:set_param value in a process-global
shadow (g_param_cache) and replays it into the next compiled script so an
operator's tuned sliders survive a hot-recompile. That shadow was only ever
cleared by cmd:unload_script — NOT on the open_project / close_project project
boundary. Since open_project does not unload the inspection script DLL, the
cache (and the persisted xi::state) leaked across a project switch: opening a
DIFFERENT recipe would silently inherit the previous recipe's tuned Param
values, mis-verdicting the vision line.

This test:
  1. open project A, compile its inspect.cpp (declares Param<int> thresh=50)
  2. set_param thresh=200  -> assert effective value is now 200
  3. open project B, compile ITS inspect.cpp (also declares thresh=50)
  4. assert B's effective thresh == 50 (its own default), NOT 200

Effective values are read via cmd:list_params, which reports each Param's live
value from the script DLL's registry.

PASS = step 4 reads 50. Before the fix it reads 200 (A's value leaked into B).

Run:  python driver.py

TODO(linux): backend compile (cl.exe) is Windows-only; SKIPs on non-nt.
"""
from __future__ import annotations
import os, socket, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{SUF}"
PORT = 7887


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout):
            return True
    except OSError:
        return False


def effective_thresh(c: Client) -> int | None:
    """Read the live value of Param 'thresh' via cmd:list_params.

    list_params acks via the rsp then ships the payload on the separate
    `instances` text channel (the SDK surfaces it as an event named
    'instances'). Drain stale events first so we read THIS call's payload.
    """
    c._drain(c._inbox_events)
    c.call("list_params")
    deadline = time.time() + 10
    while time.time() < deadline:
        ev = c._inbox_events.get(timeout=10)
        if ev.get("name") != "instances":
            continue
        params = ev.get("data", {}).get("params", [])
        for p in params:
            if p.get("name") == "thresh":
                return p.get("value")
        return None
    return None


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend compile is Windows-only here")
        return 0
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")

    projA = ROOT / "projA"
    projB = ROOT / "projB"

    blog = open(ROOT / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={PORT}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    after_set = after_switch = None
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(PORT):
            if be.poll() is not None:
                raise SystemExit(f"FAIL: backend exited during boot rc={be.poll()}")
            time.sleep(0.2)
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60) as c:
            # --- Project A: tune thresh up to 200 ---
            c.open_project(str(projA), timeout=300)
            c.compile_and_load(str(projA / "inspect.cpp"), timeout=180)
            c.set_param("thresh", 200)
            after_set = effective_thresh(c)

            # --- Switch to Project B (a different recipe) ---
            c.open_project(str(projB), timeout=300)
            c.compile_and_load(str(projB / "inspect.cpp"), timeout=180)
            after_switch = effective_thresh(c)
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        try:
            be.wait(timeout=6)
        except subprocess.TimeoutExpired:
            be.kill()
        blog.close()

    print(f"[A] effective thresh after set_param 200 = {after_set}")
    print(f"[B] effective thresh after project switch = {after_switch}")
    fails = []
    if after_set != 200:
        fails.append(f"set_param didn't take in A: thresh={after_set} (expected 200)")
    if after_switch != 50:
        fails.append(
            f"project B inherited A's tuned value: thresh={after_switch} "
            f"(expected 50 = B's own default; 200 = the cross-project leak)")

    print("=" * 48)
    if fails:
        print("VERDICT: FAIL")
        for f in fails:
            print("  -", f)
        return 1
    print("VERDICT: PASS — project B kept its own default; no cross-project param leak")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
