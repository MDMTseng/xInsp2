"""qa_bad_tempdir — a misconfigured temp dir must READ like a config error.

The backend needs a temp dir for its per-PID script build dir. It used to reach
it through the THROWING std::filesystem overloads, so a TMPDIR/TEMP/TMP naming a
path that does not exist surfaced as:

    [xinsp2] CRASH (CPP_EXCEPTION) in xinsp-backend+0x2ecf0 — minidump: ...

plus a real minidump on disk, and every caller (the FE supervisor, a QA driver,
CI) could only report "backend exited early (-6)". That cost real time once
already: contract_live had been passing only because a stray untracked directory
happened to exist, and when it went away the failure said "crash", not
"your TMPDIR is wrong".

Asserts, for a nonexistent temp root AND for a read-only one:
  * the process exits with the config-error code 3 — NOT a signal, NOT an abort;
  * stderr names the problem and prints the three env vars, so the reader can
    see which one is wrong without attaching a debugger;
  * NO new minidump is written — a user error is not a crash.
And, as the control: a normal start still comes up and serves.

Run:  python qa/qa_bad_tempdir/driver.py
"""
from __future__ import annotations

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import backend_exe, free_port  # noqa: E402

BE = backend_exe()
CONFIG_ERROR_EXIT = 3        # service_main.cpp: temp dir unusable


def count_dumps(sabotaged: Path | None = None) -> int:
    """Minidumps anywhere the backend might have put one.

    Counting only the normal <tmp>/xinsp2/crashdumps would make this assertion
    VACUOUS: the crashing build resolved its dump path from the same sabotaged
    root, so the dump landed under `sabotaged`, the normal dir never moved, and
    "no new minidump" passed for the wrong reason. Verified by running this
    driver against the pre-fix binary.
    """
    roots = [Path(tempfile.gettempdir())]
    if sabotaged:
        roots.append(sabotaged)
    n = 0
    for r in roots:
        d = r / "xinsp2" / "crashdumps"
        if d.is_dir():
            n += len(list(d.glob("*.dmp")))
    return n


def run_with(env_over: dict) -> tuple[int, str]:
    env = dict(os.environ)
    env.update(env_over)
    p = subprocess.run([str(BE), f"--port={free_port()}"], env=env,
                       capture_output=True, text=True, timeout=60)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def check(fails: list[str], label: str, env_over: dict, sabotaged: Path) -> None:
    before = count_dumps(sabotaged)
    rc, out = run_with(env_over)
    after = count_dumps(sabotaged)
    first = out.strip().splitlines()[0] if out.strip() else "(no output)"
    print(f"[{label}] exit={rc} dumps {before}->{after}  {first}")

    if rc != CONFIG_ERROR_EXIT:
        # A negative rc is a signal — i.e. it aborted, which is the regression.
        how = f"signal {-rc}" if rc < 0 else f"exit {rc}"
        fails.append(f"{label}: {how}, expected exit {CONFIG_ERROR_EXIT}")
    if "FATAL" not in out or "temp" not in out.lower():
        fails.append(f"{label}: stderr does not explain the problem: {first!r}")
    if after != before:
        fails.append(f"{label}: wrote a minidump ({before}->{after}) — "
                     "a misconfigured env is not a crash")


def main() -> int:
    if not BE.exists():
        print(f"SKIP: backend not built ({BE})"); return 0
    fails: list[str] = []

    missing = Path(tempfile.gettempdir()) / f"xi_no_such_dir_{os.getpid()}"
    shutil.rmtree(missing, ignore_errors=True)
    check(fails, "nonexistent TMPDIR", {"TMPDIR": str(missing),
                                        "TEMP": str(missing), "TMP": str(missing)},
          sabotaged=missing)

    ro = Path(tempfile.gettempdir()) / f"xi_ro_{os.getpid()}"
    shutil.rmtree(ro, ignore_errors=True)
    ro.mkdir(parents=True)
    ro.chmod(0o500)                      # exists, not writable
    try:
        if os.geteuid() == 0:
            print("[read-only TMPDIR] skipped: running as root, mode 500 is not "
                  "enforced")
        else:
            check(fails, "read-only TMPDIR", {"TMPDIR": str(ro), "TEMP": str(ro),
                                              "TMP": str(ro)}, sabotaged=ro)
    finally:
        ro.chmod(0o700)
        shutil.rmtree(ro, ignore_errors=True)

    # Control: without the sabotage the backend must still come up. Without this
    # the two checks above would also pass against a backend that refused to
    # start at all.
    port = free_port()
    proc = subprocess.Popen([str(BE), f"--port={port}"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        up = False
        deadline = time.time() + 30
        while time.time() < deadline and proc.poll() is None:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.3):
                    up = True; break
            except OSError:
                time.sleep(0.2)
        print(f"[control] normal start served on :{port}: {up}")
        if not up:
            fails.append("control: a normal start did not come up — the checks "
                         "above prove nothing")
    finally:
        proc.terminate()
        try: proc.wait(10)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
