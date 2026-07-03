"""qa_func — FUNCTIONAL / LIFECYCLE viewpoint on the FE/BE split.

Maps to fe-be-split-test-plan.md §2 (backend headless autostart AS-I*) and
§3 FE-E9/FE-E10 (fe.json config + --help). Complements the SAFETY-focused
examples/fe_supervisor (crash storm) and examples/fe_supervisor_healthy
(FE-E2/E3) — this driver pins the *positive* lifecycle wiring: that the
right project/script gets opened/compiled/started, that a missing script
degrades to open-only without taking the BE down, that a late client can
still attach, and that the FE config surface (fe.json + CLI override +
--help) behaves.

Each case spawns its OWN xinsp-backend.exe / xinsp-fe.exe on a PRIVATE port
in the 7850-7869 band (no collision with other agents / a dev BE on 7823),
logs to a file, asserts on the log + port + (where useful) a live WS client,
then name-guarded-kills its child. Skips on non-nt.

Run from this dir:  python driver.py
Run one case:       python driver.py AS-I2     (or any case ID / substring)

TODO(linux): xinsp-fe.exe autostart + the FE supervisor are Windows-only
today (CreateProcess / Job Object / console-ctrl). The .exe suffix and the
name-checked kill are gated on os.name == "nt"; the whole driver SKIPs on
non-nt. See docs/roadmap/linux-port.md.
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
SDK = REPO_ROOT / "tools" / "xinsp2_py"
sys.path.insert(0, str(SDK))
# Shared XEX1 decoder for the `expose` plugin (the VAR replacement). The SDK is
# generic and decodes no plugin payloads; scripts now surface values/images via
# xi::use("expose").process(rec) and a consumer pulls them back here.
sys.path.insert(0, str(ROOT.parents[0] / "lib"))
from xex1 import pull_latest, subscribe  # noqa: E402
from ports import free_port  # noqa: E402


def _pull_value(c, channel, key, retries=20, delay=0.1):
    """Poll the expose `get` view for channel until `key` appears (or give up).
    Robust against the binary push racing the cmd:run rsp."""
    for _ in range(retries):
        fr = pull_latest(c, channel)
        if fr is not None:
            vals = fr.get("values") or {}
            if key in vals:
                return vals.get(key)
        time.sleep(delay)
    return None

EXE_SUFFIX = ".exe" if os.name == "nt" else ""
BACKEND_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{EXE_SUFFIX}"
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE_SUFFIX}"

NOSCRIPT_PROJ = ROOT / "proj_noscript"


# ---------------------------------------------------------------------------
# small helpers (mirror plugin_crash_forensics / fe_supervisor_healthy)
# ---------------------------------------------------------------------------
def port_open(port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_port(port: int, up: bool, budget: float) -> bool:
    deadline = time.time() + budget
    while time.time() < deadline:
        if port_open(port) == up:
            return True
        time.sleep(0.2)
    return port_open(port) == up


def safe_kill(proc: subprocess.Popen, exe_name: str) -> None:
    """Kill a child we spawned, but ONLY after confirming via tasklist that
    the pid really is the exe we expect. Per memory rule
    feedback_no_kill_unknown_node: never terminate a pid we can't positively
    identify (the user runs an unrelated node server this session goes
    through)."""
    if proc.poll() is not None:
        return
    if os.name == "nt":
        try:
            out = subprocess.check_output(
                ["tasklist", "/fi", f"pid eq {proc.pid}", "/fo", "csv", "/nh"],
                stderr=subprocess.DEVNULL,
            ).decode("utf-8", "ignore").lower()
            if exe_name.lower() not in out:
                print(f"[safe_kill] REFUSING: pid={proc.pid} is not {exe_name}; tasklist={out[:120]!r}")
                return
        except Exception:
            return
    proc.terminate()
    try:
        proc.wait(timeout=4.0)
    except subprocess.TimeoutExpired:
        proc.kill()


def spawn_backend(port: int, log_path: Path, extra: list[str]) -> subprocess.Popen:
    """Spawn xinsp-backend.exe directly (no FE) on a private port."""
    logf = open(log_path, "wb")
    return subprocess.Popen(
        [str(BACKEND_EXE), f"--port={port}", *extra],
        cwd=str(BACKEND_EXE.parent),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
    )


def spawn_fe(port: int, log_path: Path, extra: list[str], cwd: Path) -> subprocess.Popen:
    """Spawn xinsp-fe.exe (the supervisor). cwd controls fe.json discovery."""
    logf = open(log_path, "wb")
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    return subprocess.Popen(
        [str(FE_EXE), f"--port={port}", *extra],
        cwd=str(cwd),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
        creationflags=flags,
    )


def read_log(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


def wait_log(p: Path, needle: str, budget: float) -> bool:
    """Wait until `needle` appears in the log. The backend binds its WS port
    BEFORE the synchronous open/compile/start runs, so port-up != ready — the
    autostart lines (and the 'autostart: ready' marker) appear seconds later.
    Poll the log rather than racing on port-up."""
    deadline = time.time() + budget
    while time.time() < deadline:
        if needle in read_log(p):
            return True
        time.sleep(0.25)
    return needle in read_log(p)


# A backend autostart sequence can cold-compile the project plugin + script
# under cl.exe (~3-4 s each), so give the port a generous budget to come up.
PORT_UP_BUDGET = 90.0


# ===========================================================================
# Cases.  Each returns a list[str] of failures ([] == pass).
# ===========================================================================
def case_AS_I1() -> list[str]:
    """AS-I1 — backend autostart happy path. --project + --script: the BE logs
    'autostart: open_project' then 'compile_and_load', opens the project, and
    its WS port accepts."""
    f: list[str] = []
    port = free_port()
    log = ROOT / "be_as_i1.log"
    if port_open(port):
        return [f"AS-I1: :{port} already in use"]
    proc = spawn_backend(port, log, [f"--project={ROOT}", "--script=inspect.cpp"])
    try:
        # Wait for the readiness marker — guarantees the open_project +
        # compile_and_load lines are already written (port-up alone doesn't).
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            f.append("AS-I1: backend never reached 'autostart: ready'")
        txt = read_log(log)
        if "autostart: open_project" not in txt:
            f.append("AS-I1: log missing 'autostart: open_project'")
        if "autostart: compile_and_load" not in txt:
            f.append("AS-I1: log missing 'autostart: compile_and_load'")
        if not port_open(port):
            f.append("AS-I1: WS port not accepting after ready")
        if proc.poll() is not None:
            f.append(f"AS-I1: backend exited during autostart (rc={proc.returncode})")
    finally:
        safe_kill(proc, "xinsp-backend.exe")
    return f


def case_AS_I2() -> list[str]:
    """AS-I2 — --autostart-fps=N starts CONTINUOUS mode. A late client sees
    inspects completing on their own (no cmd:run), proving the timer pump is
    running.

    VAR/`vars` frames were purged from the core, so we no longer watch
    `_inbox_vars`; instead we count the `run_finished` lifecycle events the
    backend brackets every continuous tick with — unsolicited proof the pump
    is turning."""
    from xinsp2 import Client  # noqa: E402
    f: list[str] = []
    port = free_port()
    log = ROOT / "be_as_i2.log"
    if port_open(port):
        return [f"AS-I2: :{port} already in use"]
    proc = spawn_backend(port, log, [f"--project={ROOT}", "--script=inspect.cpp",
                                     "--autostart-fps=10"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            f.append("AS-I2: backend never reached 'autostart: ready'")
            return f
        txt = read_log(log)
        if "autostart: start 10 fps" not in txt:
            f.append("AS-I2: log missing 'autostart: start 10 fps'")
        # Attach a passive client and watch unsolicited run_finished events
        # accumulate. The backend emits one per continuous tick (broadcast), so
        # they land in _inbox_events without us calling run().
        with Client(url=f"ws://127.0.0.1:{port}/", timeout=30.0) as c:
            time.sleep(0.5)
            c._drain(c._inbox_events)            # zero the baseline
            time.sleep(2.0)                      # ~20 ticks at 10 fps
            got = 0
            while True:
                try:
                    ev = c._inbox_events.get_nowait()
                except Exception:
                    break
                if ev.get("name") == "run_finished":
                    got += 1
        if got < 3:
            f.append(f"AS-I2: continuous mode not pumping (saw {got} run_finished in 2s, want >=3)")
        if proc.poll() is not None:
            f.append(f"AS-I2: backend exited (rc={proc.returncode})")
    finally:
        safe_kill(proc, "xinsp-backend.exe")
    return f


def case_AS_I3() -> list[str]:
    """AS-I3 — --script override. Starting with --script=alt_inspect.cpp must
    compile THAT file, not project.json's inspect.cpp. Proven by the 'script'
    value surfaced via the `expose` plugin: alt_inspect stamps 2, the default
    stamps 1 (the script reads it back through xi::use("expose"))."""
    from xinsp2 import Client  # noqa: E402
    f: list[str] = []
    port = free_port()
    log = ROOT / "be_as_i3.log"
    if port_open(port):
        return [f"AS-I3: :{port} already in use"]
    proc = spawn_backend(port, log, [f"--project={ROOT}", "--script=alt_inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            f.append("AS-I3: backend never reached 'autostart: ready'")
            return f
        txt = read_log(log)
        if "alt_inspect.cpp" not in txt:
            f.append("AS-I3: log does not name alt_inspect.cpp as the compiled script")
        with Client(url=f"ws://127.0.0.1:{port}/", timeout=30.0) as c:
            subscribe(c, ["qa"])
            c.run(timeout=30)
            marker = _pull_value(c, "qa", "script")
        if marker != 2:
            f.append(f"AS-I3: override script not active (script value={marker}, want 2)")
        if proc.poll() is not None:
            f.append(f"AS-I3: backend exited (rc={proc.returncode})")
    finally:
        safe_kill(proc, "xinsp-backend.exe")
    return f


def case_AS_I4() -> list[str]:
    """AS-I4 — --project with NO script (open-only). The BE must log the
    'no script ... open only' notice, MUST NOT exit, and keep its WS port
    accepting so an operator can attach and drive it later."""
    from xinsp2 import Client  # noqa: E402
    f: list[str] = []
    port = free_port()
    log = ROOT / "be_as_i4.log"
    if port_open(port):
        return [f"AS-I4: :{port} already in use"]
    proc = spawn_backend(port, log, [f"--project={NOSCRIPT_PROJ}"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            f.append("AS-I4: backend never reached 'autostart: ready'")
            return f
        txt = read_log(log)
        if "open only" not in txt:
            f.append("AS-I4: log missing the 'open only' (no-script) notice")
        if "autostart: compile_and_load" in txt:
            f.append("AS-I4: BE compiled a script when none was given")
        # The BE must STAY UP and be drivable by a late client.
        time.sleep(1.0)
        if proc.poll() is not None:
            f.append(f"AS-I4: backend EXITED with no script (rc={proc.returncode}) — must stay up")
        else:
            with Client(url=f"ws://127.0.0.1:{port}/", timeout=15.0) as c:
                if not c.ping():
                    f.append("AS-I4: open-only backend did not answer ping")
    finally:
        safe_kill(proc, "xinsp-backend.exe")
    return f


def case_AS_I7() -> list[str]:
    """AS-I7 — late client attach. Autostart must not monopolise the WS port:
    a client connecting AFTER the autostart sequence can still version/ping/run."""
    from xinsp2 import Client  # noqa: E402
    f: list[str] = []
    port = free_port()
    log = ROOT / "be_as_i7.log"
    if port_open(port):
        return [f"AS-I7: :{port} already in use"]
    proc = spawn_backend(port, log, [f"--project={ROOT}", "--script=inspect.cpp"])
    try:
        # Wait for readiness (poll loop about to start) before the late client
        # connects — otherwise the handshake races the synchronous autostart.
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            f.append("AS-I7: backend never reached 'autostart: ready'")
            return f
        with Client(url=f"ws://127.0.0.1:{port}/", timeout=30.0) as c:
            if not c.version():
                f.append("AS-I7: late client got no version reply")
            if not c.ping():
                f.append("AS-I7: late client ping failed")
            subscribe(c, ["qa"])
            c.run(timeout=30)
            if _pull_value(c, "qa", "count") is None:
                f.append("AS-I7: late client run() surfaced no count value via expose")
    finally:
        safe_kill(proc, "xinsp-backend.exe")
    return f


def case_FE_E10() -> list[str]:
    """FE-E10 — xinsp-fe --help. Exits 0, prints usage, spawns no backend."""
    f: list[str] = []
    try:
        # Capture BYTES and decode utf-8/replace — the help banner is UTF-8 and
        # the console locale here is cp950; text=True would raise UnicodeDecode
        # in the reader thread and silently drop the output. (Found in round 1.)
        cp = subprocess.run([str(FE_EXE), "--help"],
                            capture_output=True, timeout=15)
    except subprocess.TimeoutExpired:
        return ["FE-E10: --help did not exit (hung) — should print usage and return"]
    out = (cp.stdout or b"").decode("utf-8", "replace") + (cp.stderr or b"").decode("utf-8", "replace")
    if cp.returncode != 0:
        f.append(f"FE-E10: --help exit code {cp.returncode}, want 0")
    if "Usage: xinsp-fe" not in out:
        f.append("FE-E10: --help did not print the usage banner")
    for flag in ("--project", "--autostart-fps", "--config"):
        if flag not in out:
            f.append(f"FE-E10: usage banner omits {flag}")
    return f


def case_FE_E9() -> list[str]:
    """FE-E9 — fe.json config honoured + CLI overrides fe.json.

    Phase A: write an fe.json in a scratch cwd carrying project/script/
      autostart_fps + a be_log path, launch the FE with NO --project flag.
      The FE must read fe.json, spawn the BE on that project, and the BE
      autostart log (be_log) shows open_project + compile_and_load + the
      configured fps.
    Phase B: keep the same fe.json but pass --autostart-fps=0 on the CLI.
      The override must win: the BE must NOT start continuous mode (no
      'autostart: start' line)."""
    f: list[str] = []
    scratch = ROOT / "_fe_cfg"
    scratch.mkdir(exist_ok=True)

    # ----- Phase A: fe.json honoured -----------------------------------
    portA = free_port()
    if port_open(portA):
        return [f"FE-E9: :{portA} already in use"]
    be_logA = scratch / "beA.log"
    fe_logA = scratch / "feA.log"
    cfg = {
        "project": str(ROOT),
        "script": "inspect.cpp",
        "autostart_fps": 8,
        "be_log": str(be_logA),
        "port": portA,
    }
    (scratch / "fe.json").write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    # Pass --port explicitly too (harmless; equals cfg) so the test is robust
    # regardless of whether fe.json's port is honoured for binding.
    feA = spawn_fe(portA, fe_logA, [], cwd=scratch)
    try:
        if not wait_log(be_logA, "autostart: ready", PORT_UP_BUDGET):
            f.append("FE-E9/A: backend never reached 'autostart: ready' from fe.json config")
        be_txt = read_log(be_logA)
        if "autostart: open_project" not in be_txt:
            f.append("FE-E9/A: fe.json project not opened (no open_project in be_log)")
        if "autostart: compile_and_load" not in be_txt:
            f.append("FE-E9/A: fe.json script not compiled (no compile_and_load)")
        if "autostart: start 8 fps" not in be_txt:
            f.append("FE-E9/A: fe.json autostart_fps=8 not honoured (no 'start 8 fps')")
    finally:
        safe_kill(feA, "xinsp-fe.exe")
        wait_port(portA, up=False, budget=10)

    # ----- Phase B: CLI overrides fe.json ------------------------------
    portB = free_port()
    if port_open(portB):
        f.append(f"FE-E9/B: :{portB} already in use")
        return f
    be_logB = scratch / "beB.log"
    fe_logB = scratch / "feB.log"
    # Same fe.json (autostart_fps=8), but CLI says fps=0 -> override wins.
    feB = spawn_fe(portB, fe_logB,
                   [f"--be-log={be_logB}", "--autostart-fps=0"], cwd=scratch)
    try:
        if not wait_log(be_logB, "autostart: ready", PORT_UP_BUDGET):
            f.append("FE-E9/B: backend never reached 'autostart: ready' under CLI override")
        be_txt = read_log(be_logB)
        if "autostart: open_project" not in be_txt:
            f.append("FE-E9/B: project (from fe.json) not opened under override run")
        if "autostart: start" in be_txt:
            f.append("FE-E9/B: --autostart-fps=0 did NOT override fe.json's 8 "
                     "(continuous mode still started)")
    finally:
        safe_kill(feB, "xinsp-fe.exe")
        wait_port(portB, up=False, budget=10)
    return f


CASES = [
    ("AS-I1", case_AS_I1),
    ("AS-I2", case_AS_I2),
    ("AS-I3", case_AS_I3),
    ("AS-I4", case_AS_I4),
    ("AS-I7", case_AS_I7),
    ("FE-E9", case_FE_E9),
    ("FE-E10", case_FE_E10),
]


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe / backend autostart are Windows-only today "
              "(see docs/roadmap/linux-port.md)")
        return 0
    if not BACKEND_EXE.exists():
        sys.exit(f"FAIL: backend exe not found: {BACKEND_EXE}\n"
                 f"build: cmake --build backend/build --config Release "
                 f"--target xinsp_backend xinsp_fe")
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}")

    selector = sys.argv[1] if len(sys.argv) > 1 else None
    selected = [(cid, fn) for cid, fn in CASES
                if selector is None or selector.upper() in cid.upper()]
    if not selected:
        sys.exit(f"no case matches {selector!r}; known: {[c for c, _ in CASES]}")

    results: dict[str, list[str]] = {}
    for cid, fn in selected:
        print(f"\n===== {cid} =====")
        try:
            fails = fn()
        except Exception as e:  # a case blowing up is itself a failure
            fails = [f"{cid}: driver raised {type(e).__name__}: {e}"]
        results[cid] = fails
        print(f"[{cid}] {'PASS' if not fails else 'FAIL'}")
        for x in fails:
            print(f"    - {x}")

    all_fail = [x for fs in results.values() for x in fs]
    print("\n" + "=" * 52)
    for cid, _ in selected:
        print(f"  {cid:8} {'PASS' if not results[cid] else 'FAIL'}")
    print("=" * 52)
    print("VERDICT:", "PASS" if not all_fail else "FAIL")
    (ROOT / "_results_qa_func.json").write_text(
        json.dumps({"pass": not all_fail, "results": results}, indent=2),
        encoding="utf-8")
    return 1 if all_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
