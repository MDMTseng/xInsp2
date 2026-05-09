"""FL r8 #2 — rapid open_project / compile / start / stop / close cycles.

Looks for: leaked threads, leaked workers (xinsp-worker.exe count must
return to baseline), stale subscriptions, RSS growth.

PASS criteria
-------------
- Backend pingable after every iter.
- xinsp-worker.exe process count returns to its baseline within 3s
  after close_project.
- RSS growth across N iters bounded (<150 MB between iter 5 and iter N).
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import (BackendProc, WS_URL, REPO_ROOT,  # noqa: E402
                     count_processes, proc_rss_mb, progress)

from xinsp2 import Client, ProtocolError  # noqa: E402

ITERS = int(os.environ.get("FUZZ_ITERS", "60"))

PROJECTS = [
    REPO_ROOT / "examples" / "multi_source_surge",
    REPO_ROOT / "examples" / "cross_proc_trigger",  # exercises isolation:process workers
]


def main() -> int:
    findings: list[dict] = []
    progress(f"open_close harness started iters={ITERS}")
    rss_samples = []
    worker_baseline = count_processes("xinsp-worker.exe")
    progress(f"open_close worker baseline={worker_baseline}")

    with BackendProc() as bp:
        backend_pid = bp.proc.pid if bp.spawned else None
        c = Client(WS_URL)
        c.connect()
        try:
            for i in range(ITERS):
                proj = PROJECTS[i % len(PROJECTS)]
                if not proj.exists():
                    findings.append({"iter": i, "kind": "project_missing",
                                     "proj": str(proj)})
                    continue

                t0 = time.perf_counter()
                try:
                    c.open_project(str(proj), timeout=120)
                    open_ok = True
                except ProtocolError as e:
                    open_ok = False
                    findings.append({"iter": i, "kind": "open_protocol_error",
                                     "proj": proj.name, "err": str(e)[:300]})
                except Exception as e:
                    open_ok = False
                    findings.append({"iter": i, "kind": "open_exception",
                                     "proj": proj.name, "exc": repr(e)[:300],
                                     "fatal": True})
                    break

                if open_ok:
                    inspect = proj / "inspect.cpp"
                    if inspect.exists():
                        try:
                            c.compile_and_load(str(inspect))
                        except Exception as e:
                            findings.append({"iter": i, "kind": "compile_failed",
                                             "exc": repr(e)[:300]})
                    try:
                        c.call("start", {"fps": 60}, timeout=10)
                        time.sleep(0.1)
                        c.call("stop", timeout=10)
                    except Exception as e:
                        findings.append({"iter": i, "kind": "start_stop_exc",
                                         "exc": repr(e)[:300]})

                    try:
                        c.call("close_project", timeout=10)
                    except Exception as e:
                        findings.append({"iter": i, "kind": "close_exc",
                                         "exc": repr(e)[:300]})

                # Liveness
                try:
                    c.ping()
                except Exception as e:
                    findings.append({"iter": i, "kind": "ping_failed",
                                     "exc": repr(e)[:300], "fatal": True})
                    break

                # Worker process count check (let workers exit; allow grace)
                workers_now = count_processes("xinsp-worker.exe")
                t_grace = time.time()
                while workers_now > worker_baseline and time.time() - t_grace < 3.0:
                    time.sleep(0.1)
                    workers_now = count_processes("xinsp-worker.exe")

                rss = proc_rss_mb(backend_pid) if backend_pid else None
                rss_samples.append(rss)

                if workers_now > worker_baseline:
                    findings.append({"iter": i, "kind": "leaked_workers",
                                     "baseline": worker_baseline,
                                     "now": workers_now,
                                     "proj": proj.name})

                dt = time.perf_counter() - t0
                if (i + 1) % 5 == 0:
                    progress(f"open_close iter {i+1}/{ITERS} "
                             f"rss={rss} workers={workers_now} dt={dt:.2f}s")

            # RSS-growth check
            if len(rss_samples) >= 10:
                early = [r for r in rss_samples[3:8] if r is not None]
                late = [r for r in rss_samples[-5:] if r is not None]
                if early and late:
                    growth = (sum(late) / len(late)) - (sum(early) / len(early))
                    findings.append({"summary": "rss_growth_mb", "value": round(growth, 1),
                                     "early_avg": round(sum(early)/len(early),1),
                                     "late_avg": round(sum(late)/len(late),1)})
                    if growth > 150:
                        findings.append({"kind": "rss_grew_excessively",
                                         "growth_mb": round(growth, 1)})
        finally:
            try: c.close()
            except Exception: pass

    out = Path(__file__).parent / "_results_open_close_cycle.json"
    out.write_text(json.dumps({"iters": ITERS,
                               "worker_baseline": worker_baseline,
                               "rss_samples": rss_samples,
                               "findings": findings}, indent=2))
    fatals = sum(1 for f in findings if f.get("fatal"))
    print(f"[open_close_cycle] done findings={len(findings)} fatal={fatals}")
    progress(f"open_close done findings={len(findings)} fatal={fatals}")
    return 0 if fatals == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
