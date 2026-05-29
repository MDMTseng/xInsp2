"""qa_fault — backend headless-autostart NEGATIVE cases (AS-I4/5/6).

A bad project on a line must degrade SAFELY: the backend logs the failure and
STAYS UP (port still accepting, a client can still attach and drive it). It
must NOT exit — exiting would take the inspector process down on a mere config
mistake, which is exactly what the FE/BE split is meant to avoid.

Cases (each launches its own xinsp-backend.exe on a private port):
  QF-I1 (AS-I5): --project=<nonexistent dir>      -> open_project fails, BE up
  QF-I2 (AS-I6): --project=badscript_project       -> compile fails, BE up
  QF-I3 (AS-I4): --project with no script/--script -> "open only", BE up

Each asserts: process still alive, port still accepts, the failure is logged,
and a freshly-connected client can do version()/ping() (and for QF-I2, open a
GOOD project + ping again — the BE is still fully usable).

Run from anywhere:  python driver_autostart_negatives.py

TODO(linux): spawns xinsp-backend.exe; the .exe suffix + name-checked kill are
gated on os.name == "nt". SKIPs on non-nt. See docs/design/linux-port.md.
"""
from __future__ import annotations

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

from xinsp2 import Client, ProtocolError  # noqa: E402

EXE_SUFFIX = ".exe" if os.name == "nt" else ""
BACKEND_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{EXE_SUFFIX}"

# Private port block 7870-7889 for qa_fault. One per case so reruns don't race.
PORT_BADDIR = 7870
PORT_BADSCRIPT = 7871
PORT_NOSCRIPT = 7872

BADSCRIPT_PROJECT = ROOT / "badscript_project"
HEAL_PROJECT = ROOT / "heal_project"  # a known-GOOD project for the QF-I2 recovery probe
NONEXISTENT_DIR = ROOT / "this_project_does_not_exist_zzz"


def port_open(port: int, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def safe_kill(proc: subprocess.Popen) -> None:
    """Kill our backend only after confirming it's xinsp-backend.
    Per memory rule feedback_no_kill_unknown_node."""
    if proc.poll() is not None:
        return
    if os.name == "nt":
        try:
            out = subprocess.check_output(
                ["tasklist", "/fi", f"pid eq {proc.pid}", "/fo", "csv", "/nh"],
                stderr=subprocess.DEVNULL,
            ).decode("utf-8", "ignore").lower()
            if "xinsp-backend.exe" not in out:
                print(f"[safe_kill] REFUSING: pid={proc.pid} is not xinsp-backend")
                return
        except Exception:
            return
    proc.terminate()
    try:
        proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()


def launch_autostart(port: int, log_path: Path, project: str | None,
                     extra: list[str] | None = None) -> subprocess.Popen:
    args = [str(BACKEND_EXE), f"--port={port}"]
    if project is not None:
        args.append(f"--project={project}")
    # autostart-fps=0 -> open+compile only, no continuous loop (we drive manually).
    args.append("--autostart-fps=0")
    if extra:
        args.extend(extra)
    logf = open(log_path, "wb")
    return subprocess.Popen(
        args, cwd=str(BACKEND_EXE.parent),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
    )


def wait_port(port: int, proc: subprocess.Popen, budget: float = 20.0) -> bool:
    deadline = time.time() + budget
    while time.time() < deadline:
        if port_open(port):
            return True
        if proc.poll() is not None:
            return False
        time.sleep(0.1)
    return False


def case(name: str, port: int, log_name: str, project: str | None,
         extra: list[str] | None, log_must_contain: list[str],
         recover_with_good_project: bool) -> list[str]:
    print(f"\n==== {name} (port {port}) ====")
    fails: list[str] = []
    log_path = ROOT / log_name
    if port_open(port):
        return [f"{name}: something already on :{port}; pick a free port"]

    proc = launch_autostart(port, log_path, project, extra)
    try:
        if not wait_port(port, proc):
            rc = proc.poll()
            # The whole point: even with a bad project the BE must open its port.
            return [f"{name}: backend did not open :{port} (rc={rc}) — "
                    f"it should stay up despite the bad project (see {log_path})"]

        # Give autostart a moment to attempt the failing open/compile.
        time.sleep(1.5)
        if proc.poll() is not None:
            fails.append(f"{name}: backend EXITED (rc={proc.returncode}) on a bad "
                         f"project — must stay up")
        if not port_open(port):
            fails.append(f"{name}: port :{port} stopped accepting — BE not usable")

        log = log_path.read_text(encoding="utf-8", errors="ignore")
        low = log.lower()
        for needle in log_must_contain:
            if needle.lower() not in low:
                fails.append(f"{name}: log missing expected text {needle!r}")
        # The bad project must NOT have produced a successful open of itself.
        if name == "QF-I1" and "project opened" in low:
            fails.append("QF-I1: log says 'project opened' for a nonexistent dir")

        # A freshly-connected client must still be able to drive the BE.
        try:
            with Client(url=f"ws://127.0.0.1:{port}/", timeout=30.0) as c:
                v = c.version()
                if not c.ping():
                    fails.append(f"{name}: ping() failed — BE wedged after the failure")
                else:
                    print(f"  late client OK: version={v}")
                if recover_with_good_project:
                    # Prove the BE is still fully functional: open a GOOD project.
                    c.open_project(str(HEAL_PROJECT), timeout=300)
                    if not c.ping():
                        fails.append(f"{name}: ping() failed after recovering a good project")
                    else:
                        print("  recovered: opened a good project + ping OK")
        except (ProtocolError, OSError, ConnectionError) as e:
            fails.append(f"{name}: late client could not drive the BE: "
                         f"{type(e).__name__}: {e}")

        print(f"  alive={proc.poll() is None}  port={port_open(port)}")
        print("  log tail:")
        for ln in log.splitlines()[-8:]:
            print("    " + ln)
    finally:
        safe_kill(proc)
    return fails


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend autostart spawn path is Windows-only (see docs/design/linux-port.md)")
        return 0
    if not BACKEND_EXE.exists():
        sys.exit(f"FAIL: backend exe not found: {BACKEND_EXE}\n"
                 f"build it: cmake --build backend/build --config Release --target xinsp_backend")

    # Make the bad-project recovery probe deterministic: clear the heal marker so
    # opening heal_project here can't trip its crash path.
    for m in HEAL_PROJECT.glob("instances/*/.crashed_once.marker"):
        try:
            m.unlink()
        except OSError:
            pass

    failures: list[str] = []
    # QF-I1 (AS-I5): nonexistent --project dir.
    failures += case("QF-I1", PORT_BADDIR, "be_baddir.log",
                     str(NONEXISTENT_DIR), None,
                     log_must_contain=["open_project"],
                     recover_with_good_project=False)
    # QF-I2 (AS-I6): project whose script won't compile.
    failures += case("QF-I2", PORT_BADSCRIPT, "be_badscript.log",
                     str(BADSCRIPT_PROJECT), None,
                     log_must_contain=["compile"],
                     recover_with_good_project=True)
    # QF-I3 (AS-I4): a project.json with no "script" key and no --script flag ->
    # the BE logs "no script ... open only" and stays up (open succeeds).
    failures += case("QF-I3", PORT_NOSCRIPT, "be_noscript.log",
                     str(ROOT / "noscript_project"), None,
                     log_must_contain=["open only"],
                     recover_with_good_project=False)

    print("\n" + "=" * 52)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  bad dir / bad script / no script all logged the failure")
        print("  and the backend stayed up + usable on its WS port.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
