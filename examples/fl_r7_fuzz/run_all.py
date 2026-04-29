"""FL r7 fuzz survey orchestrator — runs harnesses 1, 2, 4 sequentially.

Harness #3 (evil_worker) requires evil_worker.exe to be built and
the original xinsp-worker.exe to be swapped out. Run it manually:

    cmake --build backend/build --config Release --target evil_worker
    python examples/fl_r7_fuzz/harness_evil_worker_host.py

Outputs JSON results files alongside each harness; this script also
writes a `_run_all_summary.json` with the combined finding counts.

Usage:
    python examples/fl_r7_fuzz/run_all.py
    FUZZ_ITERS=200 python examples/fl_r7_fuzz/run_all.py   # smaller smoke
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PY = sys.executable

HARNESSES = [
    ("ws_cmd",       ["harness_ws_cmd.py"],      "1200"),
    ("config",       ["harness_config.py"],      "500"),
    ("emit_trigger", ["harness_emit_trigger.py"], "800"),
]


def run_one(name: str, script_args: list[str], default_iters: str) -> dict:
    iters = os.environ.get("FUZZ_ITERS", default_iters)
    print(f"\n{'=' * 60}\n[run_all] running harness '{name}' iters={iters}\n{'=' * 60}", flush=True)
    env = os.environ.copy()
    env["FUZZ_ITERS"] = iters
    t0 = time.time()
    rc = subprocess.call([PY, str(HERE / script_args[0])] + script_args[1:],
                         cwd=str(HERE.parents[1]), env=env)
    dt = time.time() - t0
    print(f"[run_all] '{name}' rc={rc} took={dt:.1f}s")

    # load result file
    result_file = HERE / f"_results_{name}.json"
    if result_file.exists():
        try:
            data = json.loads(result_file.read_text())
            return {
                "name": name, "rc": rc, "elapsed_s": round(dt, 1),
                "iters": data.get("iters"),
                "findings": len(data.get("findings", [])),
                "fatals": sum(1 for f in data.get("findings", []) if f.get("fatal")),
            }
        except Exception as e:
            return {"name": name, "rc": rc, "elapsed_s": round(dt, 1),
                    "load_err": repr(e)}
    return {"name": name, "rc": rc, "elapsed_s": round(dt, 1),
            "load_err": "result file missing"}


def main() -> int:
    summary = []
    for name, args, default_iters in HARNESSES:
        summary.append(run_one(name, args, default_iters))

    print("\n" + "=" * 60)
    print("[run_all] summary:")
    print("=" * 60)
    for s in summary:
        print(json.dumps(s))
    out = HERE / "_run_all_summary.json"
    out.write_text(json.dumps(summary, indent=2))
    print(f"\nwrote {out}")

    # exit non-zero if any harness reported fatals
    return 0 if not any(s.get("fatals", 0) for s in summary) else 1


if __name__ == "__main__":
    sys.exit(main())
