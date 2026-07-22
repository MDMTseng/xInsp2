"""r8 #1 — emit_trigger high-rate stream vs WS cmd hammer.

Topology
--------
- Open multi_source_surge (in-process trigger bus + dispatcher).
- compile_and_load inspect.cpp; cmd:start fps=200.
- Thread A: hammers `exchange_instance(..., {"command":"burst",...})` —
  drives the trigger bus hard.
- Thread B: hammers ping / version / list_params / set_param on the same
  WS connection.
- Thread C: drains events/binary so the inbox doesn't grow unbounded.

PASS criteria
-------------
- No backend crash (proc stays alive, port stays open).
- All cmds eventually return (no infinite hang).
- ping/version never fail during the storm.

Budget honours ``FUZZ_DURATION`` seconds (smoke mode runs a few sec).
"""
from __future__ import annotations

import json
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import BackendProc, WS_URL, REPO_ROOT, fuzz_duration, drain_events  # noqa: E402

from xinsp2 import Client, ProtocolError  # noqa: E402

DURATION_S = fuzz_duration(12)
PROJECT = REPO_ROOT / "qa" / "multi_source_surge"


def main() -> int:
    findings: list[dict] = []
    print(f"[emit_x_cmd] duration={DURATION_S}s", flush=True)

    with BackendProc():
        c = Client(WS_URL)
        c.connect()
        try:
            try:
                c.open_project(str(PROJECT), timeout=180)
            except Exception as e:
                findings.append({"setup": "open_project", "err": repr(e)[:300]})
                return _write(findings, fatal=True)

            inspect = PROJECT / "inspect.cpp"
            try:
                if inspect.exists():
                    c.compile_and_load(str(inspect))
            except Exception as e:
                findings.append({"setup": "compile_and_load", "err": repr(e)[:300]})

            try:
                c.call("start", {"fps": 200}, timeout=10)
            except Exception as e:
                findings.append({"setup": "start", "err": repr(e)[:300]})
                return _write(findings, fatal=True)

            stop_flag = threading.Event()
            stats = {"emits": 0, "emit_errs": 0, "cmds": 0, "cmd_errs": 0,
                     "max_emit_lat_ms": 0.0, "max_cmd_lat_ms": 0.0,
                     "ping_fail": 0, "drained": 0}

            def emitter():
                instances = ["source_steady", "source_burst", "source_variable"]
                idx = 0
                while not stop_flag.is_set():
                    inst = instances[idx % len(instances)]
                    idx += 1
                    t0 = time.perf_counter()
                    try:
                        c.exchange_instance(inst, {"command": "burst", "count": 5})
                        stats["emits"] += 1
                    except ProtocolError:
                        stats["emit_errs"] += 1
                    except Exception as e:
                        stats["emit_errs"] += 1
                        findings.append({"thread": "emit", "exc": repr(e)[:300]})
                    lat_ms = (time.perf_counter() - t0) * 1000
                    if lat_ms > stats["max_emit_lat_ms"]:
                        stats["max_emit_lat_ms"] = lat_ms
                    time.sleep(0.005)

            def commander():
                seq = 0
                while not stop_flag.is_set():
                    op = ["ping", "version", "list_params", "set_param"][seq % 4]
                    seq += 1
                    t0 = time.perf_counter()
                    try:
                        if op == "set_param":
                            c.call("set_param", {"name": "nope", "value": seq})
                        else:
                            c.call(op, timeout=10)
                        stats["cmds"] += 1
                    except ProtocolError:
                        stats["cmds"] += 1  # error rsp still counts as a rsp delivered
                    except Exception as e:
                        stats["cmd_errs"] += 1
                        if op in ("ping", "version"):
                            stats["ping_fail"] += 1
                        findings.append({"thread": "cmd", "op": op, "exc": repr(e)[:300]})
                    lat_ms = (time.perf_counter() - t0) * 1000
                    if lat_ms > stats["max_cmd_lat_ms"]:
                        stats["max_cmd_lat_ms"] = lat_ms
                    time.sleep(0.002)

            def vdrain():
                while not stop_flag.is_set():
                    stats["drained"] += drain_events(c)
                    time.sleep(0.02)

            tA = threading.Thread(target=emitter, daemon=True)
            tB = threading.Thread(target=commander, daemon=True)
            tC = threading.Thread(target=vdrain, daemon=True)
            tA.start(); tB.start(); tC.start()

            t0 = time.time()
            while time.time() - t0 < DURATION_S:
                time.sleep(1.0)
                print(f"[emit_x_cmd] t={int(time.time()-t0)}s "
                      f"emits={stats['emits']} cmds={stats['cmds']} "
                      f"cmd_errs={stats['cmd_errs']}", flush=True)

            stop_flag.set()
            tA.join(timeout=5); tB.join(timeout=5); tC.join(timeout=5)

            try:
                c.call("stop", timeout=10)
            except Exception as e:
                findings.append({"final": "stop", "exc": repr(e)[:300], "fatal": True})
            try:
                c.ping()
            except Exception as e:
                findings.append({"final": "ping_after_stop", "exc": repr(e)[:300], "fatal": True})

            findings.append({"stats": stats})
            if stats["ping_fail"] > 0:
                findings.append({"kind": "ping_or_version_failed_during_storm",
                                 "count": stats["ping_fail"], "fatal": True})
        finally:
            try:
                c.close()
            except Exception:
                pass
    return _write(findings)


def _write(findings, fatal: bool = False) -> int:
    out = Path(__file__).parent / "_results_emit_x_cmd.json"
    out.write_text(json.dumps({"findings": findings}, indent=2))
    fatals = sum(1 for f in findings if f.get("fatal")) + (1 if fatal else 0)
    print(f"[emit_x_cmd] done, findings={len(findings)} fatal={fatals}")
    return 0 if fatals == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
