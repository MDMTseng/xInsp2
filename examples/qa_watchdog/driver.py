"""qa_watchdog — per-worker watchdog hard-trip under a parallel dispatch pool.

Before: the inspect watchdog used a single deadline slot and was DISABLED when
parallelism.dispatch_threads > 1 — a wedged worker under N>1 went undetected.
Now the watchdog tracks a slot per in-flight inspect, so it protects every
worker; and on a hard trip (script ignored the cooperative-cancel grace) the
backend EXITS so the FE supervisor respawns a clean one (a forced thread kill
would leak the per-instance lock + risk heap corruption).

This driver runs the backend directly with dispatch_threads=4 and a runaway
inspect (busy loop, never polls cancel). It asserts:
  * the backend process EXITS on its own (the watchdog fired under N>1), and
  * it exits with WATCHDOG_EXIT_CODE (0x5744), and
  * be.log carries the "watchdog HARD trip" line.

Run:  python driver.py

TODO(linux): backend plugin/script compile (cl.exe) is Windows-only; SKIPs on non-nt.
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
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO_ROOT / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import backend_exe, free_port  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
# Build layout is ports.backend_exe()'s job — it probes build-linux, build/Release,
# build/Debug and a plain single-config build/, so a driver never hardcodes one.
# (This list used to omit plain build/ and so missed a Ninja `-B backend/build`.)
BE = backend_exe()
PORT = free_port()  # ephemeral (was 7873); no squatter cross-talk
WD_MS = 500
# service_main.cpp exits std::_Exit(0x5744) on a hard trip. POSIX exit codes are
# only the low 8 bits, so a Linux waitpid sees 0x44; Windows keeps the full word.
WATCHDOG_EXIT_CODE = 0x5744 if os.name == "nt" else (0x5744 & 0xFF)   # 'WD'


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout): return True
    except OSError:
        return False


def main() -> int:
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")
    if port_open(PORT):
        sys.exit(f"FAIL: something already on :{PORT}")

    log_path = ROOT / "be.log"
    blog = open(log_path, "wb")
    be = subprocess.Popen([str(BE), f"--port={PORT}", f"--watchdog={WD_MS}"],
                          cwd=str(BE.parent), stdout=blog, stderr=blog,
                          stdin=subprocess.DEVNULL)
    failures: list[str] = []
    rc = None
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(PORT):
            if be.poll() is not None:
                sys.exit(f"FAIL: backend exited during boot rc={be.poll()}")
            time.sleep(0.2)

        try:
            with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60) as c:
                c.open_project(str(ROOT), timeout=300)
                c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=180)
                c.call("start", {"fps": 10})   # workers pick up ticks and wedge
        except Exception as e:
            # The backend may exit mid-call once it hard-trips; that's expected.
            print(f"[note] client call interrupted (backend likely exiting): {e}")

        # The backend should hard-trip and exit on its own: WD_MS + ~1000ms grace.
        try:
            rc = be.wait(timeout=15)
        except subprocess.TimeoutExpired:
            failures.append("backend did NOT exit — watchdog never hard-tripped under N>1")
            be.kill()
    finally:
        blog.close()

    log = log_path.read_text(encoding="utf-8", errors="ignore") if log_path.exists() else ""
    # Windows surfaces the exit code unsigned; normalize for comparison.
    rc_u = (rc & 0xFFFFFFFF) if isinstance(rc, int) else rc
    print(f"[result] exit_code={rc} (0x{rc_u:X})" if isinstance(rc, int) else f"[result] exit_code={rc}")

    if rc is None:
        pass  # already flagged
    elif rc_u != WATCHDOG_EXIT_CODE:
        failures.append(f"backend exited with {rc_u:#x}, expected watchdog code {WATCHDOG_EXIT_CODE:#x}")
    if "watchdog HARD trip" not in log:
        failures.append("be.log missing 'watchdog HARD trip' line")
    if "cooperative cancel" not in log:
        # the soft phase should have been attempted first
        print("[note] no cooperative-cancel attempt logged (acceptable, but unexpected)")

    print("---- be.log tail ----")
    for line in log.splitlines()[-12:]:
        print("  " + line)
    print("---------------------")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures: print("  -", f)
    else:
        print("VERDICT: PASS")
        print("  watchdog fired under dispatch_threads=4 and hard-tripped ->")
        print(f"  backend exited 0x{WATCHDOG_EXIT_CODE:X} for the FE to respawn.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
