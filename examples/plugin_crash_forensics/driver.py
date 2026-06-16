"""plugin_crash_forensics — regression for the FE/BE in-process model.

Process isolation was removed (2026-05): every plugin runs in-process,
so a hard plugin crash takes the whole backend down. The replacement
safety net is crash forensics — a minidump + a JSON crash report whose
threads[] array preserves each dispatch thread's breadcrumb. This driver
proves that net actually catches a real, uncatchable plugin crash.

What it does
------------
1. Spawns its OWN backend on a private port (so it can watch it die and
   read the dump without disturbing a dev backend on :7823).
2. Snapshots the crashdump directory, then opens the project + compiles.
3. Happy run: the "crasher" instance returns a count -> backend healthy,
   dispatch-thread breadcrumb stamped to last_phase="inspect".
4. Arms the instance (exchange), then runs again. The plugin spawns a
   raw std::thread that null-derefs; the access violation escapes the
   per-thread SEH translator (which only covers managed dispatch threads)
   straight to the host's SetUnhandledExceptionFilter.
5. Asserts the backend process actually EXITED, then locates the fresh
   crash report and checks it carries good debug info:
     - exception.name == ACCESS_VIOLATION
     - a non-empty minidump (.dmp) sits beside the .json
     - threads[] includes the dispatch thread's breadcrumb
       (last_phase == "inspect"), i.e. the report names the lifecycle
       stage the backend died in.

Acceptance (mechanical): all of the above hold -> PASS.

Run from this dir:  python driver.py

TODO(linux): this exercises Windows minidump forensics
(MiniDumpWriteDump) and spawns xinsp-backend.exe. The crashdump path +
the .exe suffix + the name-checked kill are gated on os.name == "nt".
On Linux the equivalent is a core dump + /proc/<pid>; see
docs/roadmap/linux-port.md.
"""
from __future__ import annotations

import json
import os
import re
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
PORT = 7836
WS_URL = f"ws://127.0.0.1:{PORT}/"


def port_open(port: int = PORT, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def safe_kill(proc: subprocess.Popen) -> None:
    """Kill our backend, but only after confirming it really is ours.
    Per memory rule feedback_no_kill_unknown_node: never kill a PID we
    can't positively identify as xinsp-backend."""
    if proc.poll() is not None:
        return
    if os.name == "nt":
        try:
            out = subprocess.check_output(
                ["tasklist", "/fi", f"pid eq {proc.pid}", "/fo", "csv", "/nh"],
                stderr=subprocess.DEVNULL,
            ).decode("utf-8", "ignore").lower()
            if "xinsp-backend.exe" not in out:
                print(f"[safe_kill] REFUSING: pid={proc.pid} is not xinsp-backend; tasklist={out[:120]!r}")
                return
        except Exception:
            return
    proc.terminate()
    try:
        proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()


def spawn_backend(log_path: Path) -> subprocess.Popen:
    if not BACKEND_EXE.exists():
        sys.exit(f"FAIL: backend exe not found: {BACKEND_EXE}\n"
                 f"build it first (cmake --build backend/build --config Release --target xinsp_backend)")
    if port_open():
        sys.exit(f"FAIL: something is already listening on :{PORT}; pick a free port")
    logf = open(log_path, "wb")
    proc = subprocess.Popen(
        [str(BACKEND_EXE), f"--port={PORT}"],
        cwd=str(BACKEND_EXE.parent),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
    )
    deadline = time.time() + 15.0
    while time.time() < deadline:
        if port_open():
            print(f"[backend] up pid={proc.pid} port={PORT}")
            return proc
        if proc.poll() is not None:
            sys.exit(f"FAIL: backend exited prematurely rc={proc.returncode} (see {log_path})")
        time.sleep(0.1)
    safe_kill(proc)
    sys.exit("FAIL: backend did not open WS port within 15s")


_DMP_RE = re.compile(r"minidump:\s*(.+?\.dmp)\s*$", re.IGNORECASE)


def find_crash_report(log_path: Path) -> tuple[Path, Path] | None:
    """Resolve the crash report from the backend's own log.

    The backend prints the absolute minidump path on crash. We parse it
    rather than guessing the crashdump dir, because the backend resolves
    its temp dir independently — assuming the driver and backend share
    %TEMP% is fragile (e.g. sandboxed shells inject a per-tool TEMP that
    grandchild processes don't inherit). The .json crash report is the
    minidump's sibling with a .json extension."""
    try:
        text = log_path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None
    matches = _DMP_RE.findall(text)
    if not matches:
        return None
    dmp = Path(matches[-1].strip())
    rpt = dmp.with_suffix(".json")
    return (rpt, dmp) if rpt.exists() else None


def main() -> int:
    log_path = ROOT / "backend.log"
    proc = spawn_backend(log_path)
    summary: dict = {"port": PORT}
    failures: list[str] = []

    try:
        logs: list[dict] = []
        with Client(url=WS_URL, timeout=60.0) as c:
            c.on_log(lambda m: logs.append(m))

            print(f"opening project {ROOT}")
            proj = c.open_project(str(ROOT), timeout=300)
            inst = [(i.get("name"), i.get("plugin")) for i in proj.get("instances", [])]
            print(f"  instances: {inst}")
            c.compile_and_load(str(ROOT / "inspect.cpp"))

            # 1) Happy run — proves the pipeline works + breadcrumb stamps.
            r = c.run(timeout=30)
            happy_count = r.value("count", None)
            print(f"happy run -> count={happy_count} ms={r.ms}")
            if happy_count is None or int(happy_count) < 0:
                failures.append(f"happy run did not return a count (got {happy_count})")
            if not c.ping():
                failures.append("backend not responsive after happy run")

            # 2) Arm, then crash.
            c.exchange_instance("crasher", {"command": "arm"})
            print("armed; issuing the crash run (backend expected to die)...")
            crashed_cleanly = False
            try:
                c.run(timeout=15)
                failures.append("crash run returned normally — plugin did NOT crash the backend")
            except (ProtocolError, OSError, ConnectionError, Exception) as e:
                crashed_cleanly = True
                print(f"  crash run raised (expected): {type(e).__name__}: {e}")
            summary["crash_run_raised"] = crashed_cleanly

        # 3) The backend process must actually have died.
        deadline = time.time() + 10.0
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.2)
        rc = proc.poll()
        summary["backend_exited"] = rc is not None
        summary["backend_returncode"] = rc
        print(f"backend exited: {rc is not None} (rc={rc})")
        if rc is None:
            failures.append("backend process is still alive — the crash was absorbed, not fatal")

        # 4) Locate the fresh crash report (via the backend's own log) and
        #    validate the forensics.
        found = find_crash_report(log_path)
        if not found:
            failures.append("backend log names no minidump / sibling .json crash report")
        else:
            rpt_path, dmp_path = found
            report = json.loads(rpt_path.read_text(encoding="utf-8"))
            summary["report_file"] = str(rpt_path)

            exc = (report.get("exception") or {}).get("name")
            summary["exception"] = exc
            if exc != "ACCESS_VIOLATION":
                failures.append(f"exception.name = {exc!r}, expected ACCESS_VIOLATION")

            dmp_size = dmp_path.stat().st_size if dmp_path.exists() else 0
            summary["minidump"] = dmp_path.name
            summary["minidump_bytes"] = dmp_size
            if dmp_size < 4096:
                failures.append(f"minidump missing or too small ({dmp_size} bytes): {dmp_path}")

            threads = report.get("threads") or []
            summary["thread_count"] = len(threads)
            phases = [t.get("last_phase") for t in threads]
            cmds = [t.get("last_cmd") for t in threads]
            summary["thread_phases"] = phases
            summary["thread_cmds"] = cmds
            if not threads:
                failures.append("crash report threads[] is empty — no breadcrumb survived")
            elif "inspect" not in phases and "inspect" not in cmds:
                failures.append(
                    f"no dispatch-thread breadcrumb names the inspect phase "
                    f"(phases={phases}, cmds={cmds})")

            ctx = report.get("context") or {}
            summary["context"] = ctx
            print(f"crash report {rpt_path.name}:")
            print(f"  exception  : {exc}")
            print(f"  minidump   : {dmp_path.name} ({dmp_size} bytes)")
            print(f"  threads    : {len(threads)}  phases={phases}  cmds={cmds}")
            print(f"  context    : {ctx}")

    finally:
        safe_kill(proc)

    # ---- Phase 2: CRT abort() path (robustness BUG 1) --------------------
    # std::abort() / a CRT fastfail bypasses SetUnhandledExceptionFilter AND
    # std::set_terminate, so it used to kill the backend with NO minidump /
    # .json sidecar — defeating crash_reports + the FE crash-history. The
    # SIGABRT/invalid-parameter/purecall handlers must now produce the same
    # forensic artifacts. Drive it with --test-abort and assert the report.
    abort_log = ROOT / "backend_abort.log"
    try:
        with open(abort_log, "w", encoding="utf-8") as lf:
            ap = subprocess.run([str(BACKEND_EXE), "--test-abort"],
                                stdout=lf, stderr=subprocess.STDOUT, timeout=30)
        print(f"--test-abort exited rc={ap.returncode}")
        rep = find_crash_report(abort_log)
        if rep is None:
            failures.append("abort() produced NO crash report (BUG 1 not fixed)")
        else:
            rpt_path, dmp_path = rep
            try:
                report = json.loads(rpt_path.read_text(encoding="utf-8"))
            except Exception as e:  # noqa: BLE001
                report = {}
                failures.append(f"abort crash report unreadable: {e}")
            exc = report.get("exception", {})
            ctx = report.get("context", {})
            summary["abort"] = {"exception": exc, "context": ctx,
                                "dmp": dmp_path.name}
            print(f"abort crash report {rpt_path.name}: exception={exc} ctx_cmd={ctx.get('last_cmd')}")
            if exc.get("name") != "CXX_ABORT":
                failures.append(f"abort report exception.name != CXX_ABORT: {exc.get('name')!r}")
            if ctx.get("last_cmd") != "abort":
                failures.append(f"abort report context.last_cmd != 'abort': {ctx.get('last_cmd')!r}")
            if not dmp_path.exists() or dmp_path.stat().st_size < 1024:
                failures.append(f"abort minidump missing/too small: {dmp_path}")
    except subprocess.TimeoutExpired:
        failures.append("--test-abort hung (did not exit within 30s)")

    # ---- Phase 3: stack-overflow path (robustness BUG 2) -----------------
    # A STACK_OVERFLOW leaves the unhandled-exception filter no stack to run on,
    # so it used to die with no dump. reserve_fault_stack() (SetThreadStackGuarantee)
    # gives the filter headroom — assert a STACK_OVERFLOW report now appears.
    so_log = ROOT / "backend_stackoverflow.log"
    try:
        with open(so_log, "w", encoding="utf-8") as lf:
            sp = subprocess.run([str(BACKEND_EXE), "--test-stackoverflow"],
                                stdout=lf, stderr=subprocess.STDOUT, timeout=30)
        print(f"--test-stackoverflow exited rc={sp.returncode}")
        rep = find_crash_report(so_log)
        if rep is None:
            failures.append("stack overflow produced NO crash report (BUG 2 not fixed)")
        else:
            rpt_path, dmp_path = rep
            try:
                report = json.loads(rpt_path.read_text(encoding="utf-8"))
            except Exception as e:  # noqa: BLE001
                report = {}
                failures.append(f"stackoverflow crash report unreadable: {e}")
            exc = report.get("exception", {})
            summary["stackoverflow"] = {"exception": exc, "dmp": dmp_path.name}
            print(f"stackoverflow crash report {rpt_path.name}: exception={exc}")
            if exc.get("name") != "STACK_OVERFLOW":
                failures.append(f"report exception.name != STACK_OVERFLOW: {exc.get('name')!r}")
            if not dmp_path.exists() or dmp_path.stat().st_size < 1024:
                failures.append(f"stackoverflow minidump missing/too small: {dmp_path}")
    except subprocess.TimeoutExpired:
        failures.append("--test-stackoverflow hung (did not exit within 30s)")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  uncatchable plugin crash -> backend died -> minidump +")
        print("  crash report with surviving inspect-phase breadcrumb;")
        print("  CRT abort() -> CXX_ABORT report; stack overflow -> STACK_OVERFLOW report.")
    (ROOT / "_results_crash_forensics.json").write_text(
        json.dumps({"pass": not failures, "failures": failures, "summary": summary},
                   indent=2), encoding="utf-8")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
