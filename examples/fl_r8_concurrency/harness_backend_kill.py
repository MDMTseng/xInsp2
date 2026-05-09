"""FL r8 #4 — backend killed mid-RPC; do worker children orphan?

Procedure
---------
1. Spawn backend manually (NOT via BackendProc, since we need to kill it).
2. Open cross_proc_trigger which has isolation:process instances → spawns
   xinsp-worker.exe children.
3. Note the pid set of those workers.
4. taskkill /F the backend (verified image name first).
5. Within 5s, every worker pid should have exited.

PASS criteria
-------------
- Workers exit cleanly within 5s of backend death.
- No orphan xinsp-worker.exe lingering > baseline.

Repeat ITERS times to catch flakiness.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import (BACKEND_EXE, WS_URL, REPO_ROOT, port_open,  # noqa: E402
                     count_processes, safe_taskkill, progress)

from xinsp2 import Client  # noqa: E402

ITERS = int(os.environ.get("FUZZ_ITERS", "5"))
PROJECT = REPO_ROOT / "examples" / "cross_proc_trigger"
KILL_GRACE_S = float(os.environ.get("KILL_GRACE_S", "5.0"))


def list_worker_pids() -> set[int]:
    if os.name != "nt":
        return set()
    try:
        out = subprocess.check_output(
            ["tasklist", "/fi", "imagename eq xinsp-worker.exe",
             "/fo", "csv", "/nh"],
            stderr=subprocess.DEVNULL,
        )
        text = out.decode("utf-8", errors="ignore").strip()
        if not text or "No tasks" in text:
            return set()
        import csv, io
        pids = set()
        for row in csv.reader(io.StringIO(text)):
            if len(row) >= 2:
                try: pids.add(int(row[1].strip()))
                except ValueError: pass
        return pids
    except Exception:
        return set()


def main() -> int:
    findings: list[dict] = []
    progress(f"backend_kill started iters={ITERS}")

    if not PROJECT.exists():
        findings.append({"setup": "missing_project", "fatal": True})
        return _write(findings)

    baseline_workers = list_worker_pids()
    progress(f"backend_kill baseline_workers={len(baseline_workers)}")

    for i in range(ITERS):
        if port_open():
            findings.append({"iter": i, "kind": "port_already_open_skipped"})
            time.sleep(2.0)
            continue

        # spawn backend
        proc = subprocess.Popen(
            [str(BACKEND_EXE)], cwd=str(BACKEND_EXE.parent),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        try:
            t0 = time.time()
            while time.time() - t0 < 15.0:
                if port_open(): break
                if proc.poll() is not None:
                    findings.append({"iter": i, "kind": "backend_died_pre_open",
                                     "rc": proc.returncode, "fatal": True})
                    raise RuntimeError("backend exited pre-open")
                time.sleep(0.1)
            else:
                findings.append({"iter": i, "kind": "no_listen", "fatal": True})
                continue

            c = Client(WS_URL); c.connect()
            try:
                c.open_project(str(PROJECT), timeout=120)
            except Exception as e:
                findings.append({"iter": i, "kind": "open_project_failed",
                                 "exc": repr(e)[:300]})
                # Even if open failed, some workers may have spawned. Continue
                # to the kill phase to validate cleanup.

            time.sleep(0.5)
            workers_before = list_worker_pids() - baseline_workers
            try: c.close()
            except Exception: pass

            if not workers_before:
                findings.append({"iter": i, "kind": "no_workers_spawned",
                                 "note": "isolation:process likely not active; cleanup test n/a"})
                # still kill the backend
            killed = safe_taskkill(proc.pid)
            if not killed:
                findings.append({"iter": i, "kind": "kill_refused", "fatal": True})
                continue
            t_kill = time.time()
            try: proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired: pass

            # poll for worker death
            still = workers_before
            t_dead = None
            while time.time() - t_kill < KILL_GRACE_S:
                cur = list_worker_pids()
                still = workers_before & cur  # those of ours still around
                if not still:
                    t_dead = time.time() - t_kill
                    break
                time.sleep(0.1)

            rec = {
                "iter": i,
                "workers_at_kill": sorted(workers_before),
                "still_alive_after_grace": sorted(still),
                "cleanup_lat_s": round(t_dead, 2) if t_dead is not None else None,
            }
            if still:
                rec["fatal"] = True
                rec["kind"] = "orphan_workers"
                # CLEANUP: kill orphans ourselves so we don't pollute
                # subsequent iters or the user's machine.
                for pid in still:
                    safe_taskkill(pid)
            findings.append(rec)
            progress(f"backend_kill iter {i+1}/{ITERS} "
                     f"workers_killed={len(workers_before) - len(still)}/{len(workers_before)} "
                     f"cleanup_s={t_dead}")
        finally:
            if proc.poll() is None:
                safe_taskkill(proc.pid)
                try: proc.wait(timeout=3.0)
                except Exception: pass

        # extra grace between iters
        time.sleep(1.0)

    return _write(findings)


def _write(findings) -> int:
    out = Path(__file__).parent / "_results_backend_kill.json"
    out.write_text(json.dumps({"findings": findings}, indent=2))
    fatals = sum(1 for f in findings if f.get("fatal"))
    print(f"[backend_kill] done findings={len(findings)} fatal={fatals}")
    progress(f"backend_kill done findings={len(findings)} fatal={fatals}")
    return 0 if fatals == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
