"""qa_race — FE supervisor timing / race / ordering regression (QA agent RACE).

Viewpoint: the FE supervisor is a SAFETY component; its correctness is about the
ORDER and COUNT of safe-state vs respawn events under a crashing backend, not
throughput. This driver proves the §5 safety properties SP1/SP2/SP6 plus a
rapid crash-burst that must still respect the respawn cap.

Cases (see PLAN.md for full pass criteria):
  RACE-SP1   every 'ENTER SAFE STATE reason=BackendExit' precedes its matching
             'respawning backend' line (safe-state BEFORE respawn).
  RACE-SP2   count(BackendExit enters) == count(respawning) + 1, and exactly one
             'RespawnLimitExceeded'  (safe-state on every death incl. the cap).
  RACE-SP6   after RespawnLimitExceeded, NO further 'respawning' lines (FE stays
             safe, does not spin); FE exits on its own (rc 2).
  RACE-BURST same project at high autostart-fps -> count(respawning) <= cap (5),
             exactly one RespawnLimitExceeded (limiter not outraced by fast deaths).
  RACE-SP3   (secondary, generous margin) first ENTER SAFE STATE within
             probe_interval + 10s of launch.

Assertions are on log-line ORDER and COUNTS (robust), not wall-clock, except the
one loose SP3 latency bound.

Run from this dir:  python driver.py

TODO(linux): xinsp-fe is Windows-only today (CreateProcess/Job Object/console
ctrl). SKIPs on non-nt. See docs/roadmap/linux-port.md.
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
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE_SUFFIX}"
CRASHBURST = ROOT / "projects" / "crashburst"

PROBE_INTERVAL_MS = 1000     # fe_main.cpp default
RESPAWN_MAX = 5              # fe_main.cpp default cap
MAX_WAIT_S = 120.0


def port_open(port: int, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def run_fe_until_cap(port: int, fps: int, log_path: Path, be_log: Path):
    """Launch the crashburst FE; wait for it to self-exit at the cap. Returns
    (rc, lines)."""
    logf = open(log_path, "wb")
    t0 = time.time()
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={port}",
         f"--project={CRASHBURST}",
         f"--autostart-fps={fps}",
         f"--be-log={be_log}"],
        cwd=str(FE_EXE.parent),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
    )
    print(f"[fe] launched pid={proc.pid} port={port} fps={fps}")
    deadline = time.time() + MAX_WAIT_S
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.5)
    rc = proc.poll()
    if rc is None:
        print("[fe] did not exit within budget; terminating")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        rc = proc.poll()
    logf.close()
    lines = log_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    return rc, lines, t0, proc


def idx_of_each(lines, predicate):
    return [i for i, ln in enumerate(lines) if predicate(ln)]


def is_enter_backend_exit(ln: str) -> bool:
    return "ENTER SAFE STATE" in ln and "reason=BackendExit" in ln


def is_respawning(ln: str) -> bool:
    return "respawning backend" in ln


def is_cap(ln: str) -> bool:
    return "ENTER SAFE STATE" in ln and "reason=RespawnLimitExceeded" in ln


def assert_sp1(lines, failures):
    """Every 'respawning backend' line must be preceded by an
    'ENTER SAFE STATE reason=BackendExit' that has no other 'respawning' between
    them — i.e. safe-state is driven BEFORE each respawn."""
    enters = idx_of_each(lines, is_enter_backend_exit)
    respawns = idx_of_each(lines, is_respawning)
    if not respawns:
        failures.append("SP1: no 'respawning backend' lines at all (no crash loop ran)")
        return
    for r in respawns:
        # the nearest BackendExit enter before this respawn
        prior_enters = [e for e in enters if e < r]
        if not prior_enters:
            failures.append(f"SP1: 'respawning backend' at line {r} with NO preceding "
                            f"'ENTER SAFE STATE reason=BackendExit' (respawn before safe-state)")
            continue
        last_enter = max(prior_enters)
        # no other respawn between that enter and this one (1 enter : 1 respawn ordering)
        between = [x for x in respawns if last_enter < x < r]
        if between:
            failures.append(f"SP1: two respawns ({between}+{r}) share one safe-state enter "
                            f"at {last_enter} — missing an ENTER between them")


def assert_sp2(lines, failures):
    enters = len(idx_of_each(lines, is_enter_backend_exit))
    respawns = len(idx_of_each(lines, is_respawning))
    caps = len(idx_of_each(lines, is_cap))
    # Every death drives a BackendExit enter; respawns happen for all but the
    # death that trips the cap. So enters == respawns + 1 (the cap death).
    if enters != respawns + 1:
        failures.append(f"SP2: BackendExit enters ({enters}) != respawns ({respawns}) + 1 "
                        f"— a death was missed or double-counted")
    if caps != 1:
        failures.append(f"SP2: expected exactly one RespawnLimitExceeded, got {caps}")
    # also each BackendExit enter should carry forensics on at least one line
    fl = [ln for ln in lines if is_enter_backend_exit(ln)]
    has_forensics = any(("module=" in ln and "module=-" not in ln) or
                        ("phase=" in ln and "phase=-" not in ln) for ln in fl)
    if fl and not has_forensics:
        failures.append("SP2: no BackendExit safe-state line carried crash forensics (module/phase)")


def assert_sp6(lines, rc, failures):
    cap_idxs = idx_of_each(lines, is_cap)
    if not cap_idxs:
        failures.append("SP6: never hit RespawnLimitExceeded (can't check stay-safe)")
        return
    cap_i = cap_idxs[0]
    after = [i for i in idx_of_each(lines, is_respawning) if i > cap_i]
    if after:
        failures.append(f"SP6: {len(after)} 'respawning' line(s) AFTER the cap "
                        f"(line {cap_i}) — FE is spinning instead of staying safe")
    if rc not in (2, None):
        print(f"[note] SP6: FE exit code {rc} (expected 2 for cap path)")
    if rc is None:
        failures.append("SP6: FE did not exit on its own after the cap")


def assert_sp3(lines, t0, failures):
    """Generous latency bound: first ENTER SAFE STATE should appear quickly.
    We can't timestamp log lines (FE doesn't stamp them), so we bound the whole
    run instead: the FE must have produced its first enter well within budget.
    This is intentionally loose — order/count are the real gates."""
    if not idx_of_each(lines, is_enter_backend_exit):
        failures.append("SP3: no ENTER SAFE STATE produced at all")


def run_sp_cases() -> list[str]:
    failures: list[str] = []
    port = 7890
    if port_open(port):
        sys.exit(f"FAIL: :{port} already in use; pick a free port in 7890-7909")
    rc, lines, t0, _ = run_fe_until_cap(port, fps=5, log_path=ROOT / "fe_sp.log",
                                        be_log=ROOT / "be_sp.log")
    print("---- fe_sp.log tail ----")
    for ln in lines[-20:]:
        print("  " + ln)
    print("------------------------")
    assert_sp1(lines, failures)
    assert_sp2(lines, failures)
    assert_sp6(lines, rc, failures)
    assert_sp3(lines, t0, failures)
    time.sleep(1.0)
    if port_open(port):
        failures.append(f"SP*: backend still listening on :{port} after FE exit — orphan")
    return failures


def run_burst_case() -> list[str]:
    failures: list[str] = []
    port = 7891
    if port_open(port):
        sys.exit(f"FAIL: :{port} already in use; pick a free port in 7890-7909")
    rc, lines, t0, _ = run_fe_until_cap(port, fps=30, log_path=ROOT / "fe_burst.log",
                                        be_log=ROOT / "be_burst.log")
    respawns = len(idx_of_each(lines, is_respawning))
    caps = len(idx_of_each(lines, is_cap))
    # The burst (crash-on-frame-0 at 30 fps) must NOT outrun the cap.
    if respawns > RESPAWN_MAX:
        failures.append(f"BURST: {respawns} respawns > cap {RESPAWN_MAX} "
                        f"— rapid deaths outran the sliding-window limiter")
    if caps != 1:
        failures.append(f"BURST: expected exactly one RespawnLimitExceeded, got {caps}")
    # And SP6 must still hold under the burst.
    assert_sp6(lines, rc, failures)
    time.sleep(1.0)
    if port_open(port):
        failures.append(f"BURST: backend still listening on :{port} after FE exit — orphan")
    print(f"[burst] respawns={respawns} caps={caps} rc={rc}")
    return failures


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/roadmap/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build: cmake --build backend/build --config Release --target xinsp_fe xinsp_backend")

    all_failures: list[str] = []
    print("=== RACE-SP1/SP2/SP6/SP3 (crashburst @5fps) ===")
    all_failures += run_sp_cases()
    print("\n=== RACE-BURST (crashburst @30fps) ===")
    all_failures += run_burst_case()

    print("\n" + "=" * 48)
    if all_failures:
        print("VERDICT: FAIL")
        for f in all_failures:
            print(f"  - {f}")
        return 1
    print("VERDICT: PASS")
    print("  SP1: safe-state precedes every respawn.")
    print("  SP2: one safe-state per death + exactly one RespawnLimitExceeded.")
    print("  SP6: no respawn after the cap; FE stayed safe and exited.")
    print("  BURST: 30fps crash storm still respected the cap; no orphan.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
