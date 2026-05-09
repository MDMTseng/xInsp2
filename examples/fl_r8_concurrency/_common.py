"""Shared helpers for FL r8 concurrency-fuzz harnesses.

Re-exports BackendProc + open_ws from r7's _common.py so we don't
duplicate the spawn/teardown logic. Adds a few process-introspection
helpers needed by the r8 fuzz scope.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

# Reuse r7 helpers verbatim — load by path to avoid the obvious
# self-name clash with our own `_common`.
HERE = Path(__file__).resolve().parent
R7_COMMON = HERE.parent / "fl_r7_fuzz" / "_common.py"
import importlib.util as _ilu
_spec = _ilu.spec_from_file_location("fl_r7_common", str(R7_COMMON))
_r7 = _ilu.module_from_spec(_spec)
_spec.loader.exec_module(_r7)  # type: ignore[union-attr]
BackendProc = _r7.BackendProc
BACKEND_EXE = _r7.BACKEND_EXE
WS_URL = _r7.WS_URL
WS_HOST = _r7.WS_HOST
WS_PORT = _r7.WS_PORT
port_open = _r7.port_open
open_ws = _r7.open_ws

REPO_ROOT = HERE.parents[1]
PROGRESS_FILE = REPO_ROOT / ".fl_progress" / "fl_r8_concurrency.txt"


def progress(msg: str) -> None:
    """Append-only progress brief — read by parent cron every 10 min."""
    try:
        PROGRESS_FILE.parent.mkdir(parents=True, exist_ok=True)
        with open(PROGRESS_FILE, "a", encoding="utf-8") as f:
            f.write(f"{time.strftime('%H:%M:%S')}  {msg}\n")
    except Exception:
        pass


# TODO(linux): tasklist is Win-specific; on Linux use `pgrep -c -x` or
# read /proc. The whole orphan-worker check below needs an #ifdef-style
# branch. See docs/design/linux-port.md.
def count_processes(image_name: str) -> int:
    """Count running processes with the given image name. Windows-only."""
    if os.name != "nt":
        # Best-effort POSIX fallback; accept any error as "0".
        try:
            out = subprocess.check_output(
                ["pgrep", "-c", "-x", image_name.replace(".exe", "")],
                stderr=subprocess.DEVNULL,
            )
            return int(out.strip())
        except Exception:
            return 0
    try:
        out = subprocess.check_output(
            ["tasklist", "/fi", f"imagename eq {image_name}", "/fo", "csv", "/nh"],
            stderr=subprocess.DEVNULL,
        )
        text = out.decode("utf-8", errors="ignore").strip()
        if not text or "No tasks" in text:
            return 0
        return len([line for line in text.splitlines() if line.strip()])
    except Exception:
        return 0


def proc_rss_mb(pid: int) -> float | None:
    """Get RSS for a Windows process in MB via tasklist."""
    if os.name != "nt":
        try:
            with open(f"/proc/{pid}/statm") as f:
                rss_pages = int(f.read().split()[1])
            return rss_pages * 4096 / (1024 * 1024)
        except Exception:
            return None
    try:
        out = subprocess.check_output(
            ["tasklist", "/fi", f"pid eq {pid}", "/fo", "csv", "/nh"],
            stderr=subprocess.DEVNULL,
        )
        text = out.decode("utf-8", errors="ignore").strip()
        if not text or "No tasks" in text:
            return None
        # CSV: "image","pid","sess","sess#","mem"  ;  mem like "12,345 K"
        import csv, io
        for row in csv.reader(io.StringIO(text)):
            if len(row) >= 5:
                mem = row[4].replace(",", "").replace(" K", "").strip()
                try:
                    return float(mem) / 1024.0
                except ValueError:
                    return None
    except Exception:
        return None
    return None


def safe_taskkill(pid: int) -> bool:
    """taskkill /F /PID <pid> — but ONLY after verifying the image name
    is xinsp-backend.exe or xinsp-worker.exe. Per memory rule
    `feedback_no_kill_unknown_node.md`, never kill unknown node/npm.
    Returns True if killed."""
    if os.name != "nt":
        try:
            os.kill(pid, 9)
            return True
        except Exception:
            return False
    try:
        out = subprocess.check_output(
            ["tasklist", "/fi", f"pid eq {pid}", "/fo", "csv", "/nh"],
            stderr=subprocess.DEVNULL,
        )
        text = out.decode("utf-8", errors="ignore").lower()
        if "xinsp-backend.exe" not in text and "xinsp-worker.exe" not in text:
            print(f"[safe_taskkill] REFUSING to kill pid={pid} — not an xinsp-*.exe; tasklist={text[:200]!r}",
                  flush=True)
            return False
        subprocess.call(["taskkill", "/F", "/PID", str(pid)],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True
    except Exception as e:
        print(f"[safe_taskkill] error: {e!r}", flush=True)
        return False
