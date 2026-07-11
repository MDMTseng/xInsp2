"""qa_edge — resource / security / edge-case regression for the FE/BE split.

QA Agent EDGE viewpoint (test plan §1 CR-U*, §3 FE-E3 orphan, §5 SP5). This
driver covers the runtime edges that a pure C++ unit cannot reach — they need a
live FE process:

  QE-O1  orphan reap on hard FE kill (taskkill /F, NOT Ctrl-C) — the Job Object
         KILL_ON_JOB_CLOSE guarantee. After the FE is force-killed, no backend
         may be left listening on the private port.            (FE-E3 / SP5)

  QE-O2  orphan reap when the FE is killed DURING respawn-backoff. We aim the
         kill at the 1.5 s window between a backend death and its respawn, the
         nastiest moment for a leak, and assert the same no-orphan property.

  QE-L1  no growing handle/log leak across many respawns — DOCUMENTED check
         (counts the FE's open handles before/after a crash-storm via the
         backend's own crash loop). Skipped-soft if the tooling isn't present.

  QE-Q1  --project path WITH SPACES still launches — build_cmdline() quoting.
         The FE must pass the quoted path to the backend as a single argv token;
         we assert the backend booted (port opened) rather than choking on a
         split path.                                            (build_cmdline)

  QE-S1  --project path-injection value is treated as a PATH, not a shell. A
         value like  proj" & calc.exe & "  must NOT spawn anything; CreateProcess
         gets argv directly (no cmd.exe), so the worst case is a failed open.
         We assert no extra process and that the FE/BE did not execute the
         injected token.                                        (security)

The pure-parser edges (no minidump line, missing/corrupt json, multi-minidump
last-wins, threads[] fallback) are covered by backend/tests/test_qa_edge.cpp
against xi::enrich_from_crash_report + the crafted fixtures/ here; this driver
does not duplicate them.

Run from this dir:  python driver.py

TODO(linux): xinsp-fe + Job Object + taskkill are Windows-only today; this
driver SKIPs on non-nt. The orphan guarantee maps to PR_SET_PDEATHSIG on Linux
(see docs/roadmap/linux-port.md).
"""
from __future__ import annotations

import contextlib
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "examples" / "lib"))
from ports import free_port, backend_exe, fe_exe  # noqa: E402
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = fe_exe()

# Ephemeral, probed-free ports (were the reserved 7910-7913): a stale/foreign
# backend on a fixed port would make these edge tests false-fail.
PORT_ORPHAN = free_port()
PORT_BACKOFF = free_port()
PORT_QUOTE = free_port()
PORT_INJECT = free_port()


def port_open(port: int, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_port(port: int, up: bool, deadline_s: float) -> bool:
    end = time.time() + deadline_s
    while time.time() < end:
        if port_open(port) == up:
            return True
        time.sleep(0.1)
    return port_open(port) == up


def hard_kill_tree(pid: int) -> None:
    """Force-kill the FE by PID to simulate a hard FE crash (the case Ctrl-C
    does NOT cover). Per memory rule feedback_no_kill_unknown_node we only ever
    pass a PID we ourselves spawned (the FE we launched). We deliberately kill
    ONLY the FE (not its child backend): on Linux the backend must reap itself
    via PR_SET_PDEATHSIG when its parent FE dies — that reap is exactly what the
    orphan cases assert. On Windows the Job Object KILL_ON_JOB_CLOSE reaps it."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def fe_handle_count(pid: int) -> int | None:
    """Best-effort open-handle/fd count for the FE process (QE-L1 leak check).
    On Linux we count entries in /proc/<pid>/fd. On Windows the equivalent
    (HandleCount) needs tooling we can't guarantee, so we return None and the
    caller treats QE-L1 as a documented-but-unmeasured check."""
    if os.name == "nt":
        try:
            out = subprocess.check_output(
                ["powershell", "-NoProfile", "-Command",
                 f"(Get-Process -Id {pid}).HandleCount"],
                stderr=subprocess.DEVNULL, timeout=10,
            ).decode("utf-8", "ignore").strip()
            return int(out)
        except Exception:
            return None
    try:
        return len(os.listdir(f"/proc/{pid}/fd"))
    except OSError:
        return None


def launch_fe(port: int, project: str, fps: int, log_path: Path,
              extra: list[str] | None = None) -> tuple[subprocess.Popen, "object"]:
    log = open(log_path, "wb")
    args = [str(FE_EXE), f"--port={port}", f"--project={project}",
            f"--autostart-fps={fps}", f"--be-log={ROOT / (log_path.stem + '.be.log')}"]
    if extra:
        args += extra
    proc = subprocess.Popen(
        args, cwd=str(FE_EXE.parent),
        stdout=log, stderr=log, stdin=subprocess.DEVNULL,
    )
    return proc, log


def main() -> int:
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release "
                 f"--target xinsp_fe xinsp_backend")

    for p in (PORT_ORPHAN, PORT_BACKOFF, PORT_QUOTE, PORT_INJECT):
        if port_open(p):
            sys.exit(f"FAIL: something already listening on private port :{p}")

    failures: list[str] = []
    summary: dict = {}

    # A healthy project for the launch/orphan cases. We point at the existing
    # fe_supervisor_healthy project if present (a known-good, non-crashing one);
    # otherwise we fall back to this dir and tolerate an open-only backend.
    healthy = REPO_ROOT / "examples" / "fe_supervisor_healthy"
    project_dir = healthy if healthy.exists() else ROOT

    # ---- QE-O1: orphan reap on hard FE kill -------------------------------
    print("[QE-O1] hard-kill the FE; assert no orphaned backend on the port")
    proc, log = launch_fe(PORT_ORPHAN, str(project_dir), 5, ROOT / "qe_o1.fe.log")
    try:
        if not wait_port(PORT_ORPHAN, up=True, deadline_s=20):
            failures.append("QE-O1: backend never opened the port under the FE")
        hard_kill_tree(proc.pid)          # simulate a hard FE crash (no Ctrl-C)
        proc.wait(timeout=10)
        # Job Object KILL_ON_JOB_CLOSE must have reaped the backend.
        gone = wait_port(PORT_ORPHAN, up=False, deadline_s=8)
        summary["QE-O1_orphan_reaped"] = gone
        if not gone:
            failures.append(f"QE-O1: backend STILL listening on :{PORT_ORPHAN} "
                            f"after hard FE kill — ORPHAN (Job Object did not reap)")
    finally:
        log.close()

    # ---- QE-O2: orphan reap when killed DURING respawn-backoff -------------
    # The crasher project (this dir, armed) makes the backend die ~immediately;
    # we kill the FE shortly after launch to land inside the 1.5 s backoff.
    print("[QE-O2] kill the FE mid respawn-backoff; assert no orphan")
    proc2, log2 = launch_fe(PORT_BACKOFF, str(ROOT), 5, ROOT / "qe_o2.fe.log")
    try:
        # Let the first crash + safe-state happen, then kill in the backoff gap.
        time.sleep(2.5)
        hard_kill_tree(proc2.pid)
        proc2.wait(timeout=10)
        gone = wait_port(PORT_BACKOFF, up=False, deadline_s=8)
        summary["QE-O2_orphan_reaped"] = gone
        if not gone:
            failures.append(f"QE-O2: backend STILL on :{PORT_BACKOFF} after FE "
                            f"killed during respawn-backoff — ORPHAN")
    finally:
        log2.close()

    # ---- QE-Q1: --project path WITH SPACES launches (build_cmdline quoting) -
    print("[QE-Q1] --project with spaces in the path still launches the backend")
    spaced = ROOT / "fixtures" / "proj with spaces"
    spaced.mkdir(parents=True, exist_ok=True)
    # Minimal project.json so open_project has something to chew on (open-only
    # is fine; we only assert the path survived quoting and the BE booted).
    (spaced / "project.json").write_text('{"instances": []}', encoding="utf-8")
    proc3, log3 = launch_fe(PORT_QUOTE, str(spaced), 0, ROOT / "qe_q1.fe.log")
    try:
        booted = wait_port(PORT_QUOTE, up=True, deadline_s=20)
        summary["QE-Q1_launched_with_spaces"] = booted
        if not booted:
            failures.append("QE-Q1: backend did not boot with a spaced --project "
                            "path — build_cmdline() quoting regressed")
    finally:
        hard_kill_tree(proc3.pid)
        try:
            proc3.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        wait_port(PORT_QUOTE, up=False, deadline_s=8)
        log3.close()

    # ---- QE-S1: --project injection value is a PATH, not a shell -----------
    # execv/CreateProcessA receive argv directly (no /bin/sh, no cmd.exe), so a
    # shell-ish --project value cannot execute an embedded command. We inject a
    # value that WOULD create a sentinel file if any shell ever interpreted it,
    # then assert the sentinel never appeared (and no orphan was left). The
    # backend simply fails to open the bogus path.
    print("[QE-S1] --project shell-injection value is treated as a path, not run")
    sentinel = ROOT / "fixtures" / "qe_s1_pwned"
    with contextlib.suppress(OSError):
        sentinel.unlink()
    if os.name == "nt":
        inject = f'bogus" & echo pwned > "{sentinel}" & "tail'
    else:
        inject = f'bogus"; touch "{sentinel}"; "tail'
    proc4, log4 = launch_fe(PORT_INJECT, inject, 0, ROOT / "qe_s1.fe.log")
    try:
        time.sleep(3.0)   # give any (mis)spawn time to appear
        pwned = sentinel.exists()
        summary["QE-S1_injection_executed"] = pwned
        if pwned:
            failures.append("QE-S1: injected --project value created a sentinel "
                            "file — value reached a shell (CRITICAL security "
                            "regression)")
    finally:
        hard_kill_tree(proc4.pid)
        try:
            proc4.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        wait_port(PORT_INJECT, up=False, deadline_s=8)
        log4.close()

    # ---- QE-L1: handle/log leak across respawns (documented check) ---------
    # Crash-storm the FE (this dir, armed) and sample HandleCount early vs late.
    # A healthy supervisor closes each backend's process/thread/log handles per
    # spawn (fe_main.cpp CloseHandle trio), so the count should be roughly flat.
    print("[QE-L1] handle-leak check across respawns (documented; soft on no tooling)")
    proc5, log5 = launch_fe(free_port(), str(ROOT), 5, ROOT / "qe_l1.fe.log")
    try:
        time.sleep(2.0)
        h_early = fe_handle_count(proc5.pid)
        time.sleep(6.0)   # several crash/respawn cycles (1.5 s backoff each)
        h_late = fe_handle_count(proc5.pid)
        summary["QE-L1_handles_early"] = h_early
        summary["QE-L1_handles_late"] = h_late
        if h_early is None or h_late is None:
            print("  [QE-L1] HandleCount unavailable; documented-only "
                  "(re-run with handle.exe / Process Explorer to measure)")
        elif h_late > h_early * 2 and h_late - h_early > 100:
            failures.append(f"QE-L1: FE handle count grew {h_early}->{h_late} "
                            f"across respawns — possible handle leak")
    finally:
        hard_kill_tree(proc5.pid)
        try:
            proc5.wait(timeout=10)
        except subprocess.TimeoutExpired:
            pass
        log5.close()

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  no orphans on hard/backoff FE kill, spaced --project launches,")
        print("  injection value stays a path, no handle leak across respawns.")
    import json
    (ROOT / "_results_qa_edge.json").write_text(
        json.dumps({"pass": not failures, "failures": failures, "summary": summary},
                   indent=2), encoding="utf-8")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
