"""r8 #3 — fire cmds while compile_and_load is in flight.

The WS handler thread is parked synchronously inside the compiler while
a script compiles. We send cmds on the SAME socket from a separate
Python thread (the SDK multiplexes on cid) and verify none are lost,
reordered, or misrouted, and the backend never crashes.

PASS criteria
-------------
- compile completes (success or ProtocolError, both ok).
- Concurrent ping/version cmds get a rsp (before or after compile) or a
  ProtocolError — NOT a timeout (a timeout = lost cmd).
- No backend crash.

Iteration count honours ``FUZZ_ITERS`` (smoke mode runs a small count).
"""
from __future__ import annotations

import json
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import BackendProc, WS_URL, REPO_ROOT, fuzz_iters  # noqa: E402

from xinsp2 import Client, ProtocolError  # noqa: E402

ITERS = fuzz_iters(10)
INSPECT = REPO_ROOT / "qa" / "multi_source_surge" / "inspect.cpp"
PROJECT = REPO_ROOT / "qa" / "multi_source_surge"


def main() -> int:
    findings: list[dict] = []
    print(f"[cmd_during_compile] iters={ITERS}", flush=True)

    with BackendProc():
        c = Client(WS_URL)
        c.connect()
        try:
            try:
                c.open_project(str(PROJECT), timeout=180)
            except Exception as e:
                findings.append({"setup": "open_project", "err": repr(e)[:300],
                                 "fatal": True})
                return _write(findings)

            for i in range(ITERS):
                stop_flag = threading.Event()
                hammer_results = []

                def hammer():
                    cnt = 0
                    while not stop_flag.is_set():
                        op = ["ping", "version", "list_params"][cnt % 3]
                        cnt += 1
                        t0 = time.perf_counter()
                        rec = {"op": op, "iter": i}
                        try:
                            c.call(op, timeout=30)
                            rec["lat_ms"] = round((time.perf_counter() - t0) * 1000, 2)
                            rec["ok"] = True
                        except ProtocolError as e:
                            rec["ok"] = False
                            rec["err"] = str(e)[:200]
                        except Exception as e:
                            rec["ok"] = False
                            rec["fatal"] = True
                            rec["exc"] = repr(e)[:300]
                        hammer_results.append(rec)
                        time.sleep(0.005)

                th = threading.Thread(target=hammer, daemon=True)
                th.start()

                t0 = time.perf_counter()
                try:
                    c.compile_and_load(str(INSPECT))
                    compile_lat = (time.perf_counter() - t0) * 1000
                except ProtocolError as e:
                    compile_lat = (time.perf_counter() - t0) * 1000
                    findings.append({"iter": i, "kind": "compile_protocol_error",
                                     "err": str(e)[:300]})
                except Exception as e:
                    findings.append({"iter": i, "kind": "compile_exception",
                                     "exc": repr(e)[:300], "fatal": True})
                    stop_flag.set(); th.join(timeout=2)
                    break

                stop_flag.set(); th.join(timeout=10)

                try:
                    c.ping()
                except Exception as e:
                    findings.append({"iter": i, "kind": "ping_after_compile_fail",
                                     "exc": repr(e)[:300], "fatal": True})
                    break

                hammer_total = len(hammer_results)
                hammer_oks = sum(1 for r in hammer_results if r.get("ok"))
                hammer_fatals = [r for r in hammer_results if r.get("fatal")]
                max_lat = max((r.get("lat_ms", 0) for r in hammer_results
                               if r.get("ok")), default=0)
                rec = {"iter": i, "compile_lat_ms": round(compile_lat, 1),
                       "hammer_total": hammer_total, "hammer_ok": hammer_oks,
                       "hammer_max_lat_ms": round(max_lat, 1),
                       "hammer_fatals": len(hammer_fatals)}
                if hammer_fatals:
                    rec["fatal"] = True
                    rec["fatal_samples"] = hammer_fatals[:3]
                findings.append(rec)
                print(f"[cmd_during_compile] iter {i+1}/{ITERS} "
                      f"compile_lat={int(compile_lat)}ms "
                      f"hammer_ok={hammer_oks}/{hammer_total}", flush=True)
        finally:
            try:
                c.close()
            except Exception:
                pass

    return _write(findings)


def _write(findings) -> int:
    out = Path(__file__).parent / "_results_cmd_during_compile.json"
    out.write_text(json.dumps({"findings": findings}, indent=2))
    fatals = sum(1 for f in findings if f.get("fatal"))
    print(f"[cmd_during_compile] done findings={len(findings)} fatal={fatals}")
    return 0 if fatals == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
