"""Shared spawn/kill helpers for the robustness-diagnostic PoCs (_diag/).

Mirrors examples/qa_func/driver.py: each PoC spawns its OWN xinsp-backend.exe
on a PRIVATE port (7900-7929 band, away from the 7823 dev BE and the :9091
node hub), logs to a file, and name-guards every kill per memory rule
feedback_no_kill_unknown_node. Windows-only; SKIPs on non-nt.
"""
from __future__ import annotations
import os, socket, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
SDK = REPO_ROOT / "tools" / "xinsp2_py"
sys.path.insert(0, str(SDK))

EXE = ".exe" if os.name == "nt" else ""
BACKEND_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{EXE}"
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE}"
COMMS_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-comms{EXE}"


def port_open(port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def safe_kill(proc: subprocess.Popen, exe_name: str) -> None:
    """Terminate a child we spawned, but ONLY after tasklist confirms the pid
    really is exe_name (never kill an unidentified pid — the user runs an
    unrelated node hub this session goes through)."""
    if proc.poll() is not None:
        return
    if os.name == "nt":
        try:
            out = subprocess.check_output(
                ["tasklist", "/fi", f"pid eq {proc.pid}", "/fo", "csv", "/nh"],
                stderr=subprocess.DEVNULL,
            ).decode("utf-8", "ignore").lower()
            if exe_name.lower() not in out:
                print(f"[safe_kill] REFUSING: pid={proc.pid} != {exe_name}; tasklist={out[:120]!r}")
                return
        except Exception:
            return
    proc.terminate()
    try:
        proc.wait(timeout=4.0)
    except subprocess.TimeoutExpired:
        proc.kill()


def spawn_backend(port: int, log_path: Path, extra: list[str]) -> subprocess.Popen:
    logf = open(log_path, "wb")
    return subprocess.Popen(
        [str(BACKEND_EXE), f"--port={port}", *extra],
        cwd=str(BACKEND_EXE.parent),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
    )


def read_log(p: Path) -> str:
    try:
        return p.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


def wait_log(p: Path, needle: str, budget: float) -> bool:
    deadline = time.time() + budget
    while time.time() < deadline:
        if needle in read_log(p):
            return True
        time.sleep(0.25)
    return needle in read_log(p)


PORT_UP_BUDGET = 90.0
