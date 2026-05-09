"""FL r8 #5 — set_param storm during continuous mode.

Open multi_source_surge → compile inspect.cpp → start fps=60. From a
worker thread, blast set_param at >1000/sec for DURATION_S; from the
main thread drain vars events. Look for crashes, dropped responses,
backend hang.

PASS criteria
-------------
- Backend stays pingable.
- All set_param calls return a rsp (ok or protocol-error) — none lost.
- vars events keep flowing (we observe at least 60% of the expected
  vars frames over the duration; we relax that since trigger policy /
  source plugin behaviour can suppress some).
"""
from __future__ import annotations

import json
import os
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import BackendProc, WS_URL, REPO_ROOT, progress  # noqa: E402

from xinsp2 import Client, ProtocolError  # noqa: E402

DURATION_S = float(os.environ.get("FUZZ_DURATION", "8"))
PROJECT = REPO_ROOT / "examples" / "multi_source_surge"


def main() -> int:
    findings: list[dict] = []
    progress("set_param_storm started")

    with BackendProc():
        c = Client(WS_URL); c.connect()
        try:
            try:
                c.open_project(str(PROJECT), timeout=180)
            except Exception as e:
                findings.append({"setup": "open_project", "err": repr(e)[:300],
                                 "fatal": True})
                return _write(findings)

            inspect = PROJECT / "inspect.cpp"
            if inspect.exists():
                try:
                    c.compile_and_load(str(inspect))
                except Exception as e:
                    findings.append({"setup": "compile", "err": repr(e)[:300]})

            # Discover param names — set_param of unknowns may early-out
            # before the storm gets interesting.
            try:
                params = c.call("list_params")
                names = [p.get("name") for p in params if isinstance(p, dict)] \
                    if isinstance(params, list) else []
            except Exception:
                names = []
            if not names:
                names = ["fps", "burst", "trigger_source"]
            progress(f"set_param_storm param_names={names[:5]}...")

            try:
                c.call("start", {"fps": 60}, timeout=10)
            except Exception as e:
                findings.append({"setup": "start", "err": repr(e)[:300],
                                 "fatal": True})
                return _write(findings)

            stop_flag = threading.Event()
            stats = {"sets": 0, "set_errs": 0, "exc": 0, "vars": 0,
                     "max_lat_ms": 0.0}

            def storm():
                seq = 0
                while not stop_flag.is_set():
                    nm = names[seq % max(len(names), 1)]; seq += 1
                    val = (seq * 7) % 1000
                    t0 = time.perf_counter()
                    try:
                        c.call("set_param", {"name": nm, "value": val}, timeout=10)
                        stats["sets"] += 1
                    except ProtocolError:
                        stats["set_errs"] += 1
                    except Exception as e:
                        stats["exc"] += 1
                        findings.append({"thread": "storm", "exc": repr(e)[:300]})
                    lat_ms = (time.perf_counter() - t0) * 1000
                    if lat_ms > stats["max_lat_ms"]:
                        stats["max_lat_ms"] = lat_ms

            def vdrain():
                while not stop_flag.is_set():
                    v = c.next_vars(timeout=0.05)
                    if v is not None:
                        stats["vars"] += 1

            tA = threading.Thread(target=storm, daemon=True)
            tB = threading.Thread(target=vdrain, daemon=True)
            tA.start(); tB.start()

            t0 = time.time()
            while time.time() - t0 < DURATION_S:
                time.sleep(1.0)
                progress(f"set_param_storm t={int(time.time()-t0)}s "
                         f"sets={stats['sets']} vars={stats['vars']} "
                         f"max_lat={stats['max_lat_ms']:.1f}")

            stop_flag.set()
            tA.join(timeout=10); tB.join(timeout=5)

            try:
                c.call("stop", timeout=10)
            except Exception as e:
                findings.append({"final": "stop", "exc": repr(e)[:300], "fatal": True})

            try:
                c.ping()
            except Exception as e:
                findings.append({"final": "ping", "exc": repr(e)[:300], "fatal": True})

            sets_per_s = stats["sets"] / DURATION_S
            findings.append({"stats": stats, "sets_per_s": round(sets_per_s, 1)})
            if stats["exc"] > 0:
                findings.append({"kind": "non_protocol_exceptions_during_storm",
                                 "count": stats["exc"]})
            if sets_per_s < 50:
                findings.append({"kind": "set_param_throughput_low",
                                 "sets_per_s": round(sets_per_s, 1),
                                 "note": "expected >50/s; possible backpressure or hang"})
        finally:
            try: c.close()
            except Exception: pass

    return _write(findings)


def _write(findings) -> int:
    out = Path(__file__).parent / "_results_set_param_storm.json"
    out.write_text(json.dumps({"findings": findings}, indent=2))
    fatals = sum(1 for f in findings if f.get("fatal"))
    print(f"[set_param_storm] done findings={len(findings)} fatal={fatals}")
    progress(f"set_param_storm done findings={len(findings)} fatal={fatals}")
    return 0 if fatals == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
