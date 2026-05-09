"""FL r8 concurrency-fuzz orchestrator. Runs all 5 harnesses
sequentially, each with its own freshly-spawned backend.

Each harness writes its own _results_<name>.json. We aggregate counts
into _run_all_summary.json.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from _common import progress  # noqa: E402

HERE = Path(__file__).resolve().parent
PY = sys.executable

HARNESSES = [
    # name, script, env-var defaults
    ("emit_x_cmd",         "harness_emit_x_cmd.py",        {"FUZZ_DURATION": "12"}),
    ("open_close_cycle",   "harness_open_close_cycle.py",  {"FUZZ_ITERS": "30"}),
    ("cmd_during_compile", "harness_cmd_during_compile.py",{"FUZZ_ITERS": "8"}),
    ("set_param_storm",    "harness_set_param_storm.py",   {"FUZZ_DURATION": "8"}),
    ("backend_kill",       "harness_backend_kill.py",      {"FUZZ_ITERS": "4"}),
]


def run_one(name: str, script: str, env_defaults: dict) -> dict:
    print(f"\n{'='*64}\n[run_all] running '{name}'\n{'='*64}", flush=True)
    progress(f"run_all running {name}")
    env = os.environ.copy()
    for k, v in env_defaults.items():
        env.setdefault(k, v)
    t0 = time.time()
    rc = subprocess.call([PY, str(HERE / script)], cwd=str(HERE.parents[1]),
                         env=env)
    dt = time.time() - t0
    print(f"[run_all] '{name}' rc={rc} took={dt:.1f}s", flush=True)

    result_file = HERE / f"_results_{name}.json"
    summary = {"name": name, "rc": rc, "elapsed_s": round(dt, 1)}
    if result_file.exists():
        try:
            data = json.loads(result_file.read_text())
            findings = data.get("findings", [])
            summary["findings"] = len(findings)
            summary["fatals"] = sum(1 for f in findings if f.get("fatal"))
        except Exception as e:
            summary["load_err"] = repr(e)
    else:
        summary["load_err"] = "result file missing"
    return summary


def main() -> int:
    progress("run_all started")
    summary = []
    for name, script, env_defaults in HARNESSES:
        summary.append(run_one(name, script, env_defaults))

    print("\n" + "=" * 64)
    print("[run_all] SUMMARY")
    print("=" * 64)
    for s in summary:
        print(json.dumps(s))
    out = HERE / "_run_all_summary.json"
    out.write_text(json.dumps(summary, indent=2))
    print(f"\nwrote {out}")
    progress(f"run_all done fatals={sum(s.get('fatals', 0) for s in summary)}")
    return 0 if not any(s.get("fatals", 0) for s in summary) else 1


if __name__ == "__main__":
    sys.exit(main())
