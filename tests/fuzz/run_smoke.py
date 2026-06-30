"""Smoke entry for the salvaged r7/r8 fuzz harnesses.

Runs each kept harness at a reduced iteration / duration budget so the
whole suite finishes in well under a couple of minutes — intended to be
wired into CI as a fast, build-breaking net (core_fix_plan.md §23 T0.2,
§24, §27.4).

Each harness is launched as its own subprocess and (via _common's
``BackendProc``) spawns + tears down its own backend, so a crash in one
harness can't contaminate the next. Each writes `_results_<name>.json`
next to itself; this script aggregates fatal counts into
`_run_smoke_summary.json` and exits non-zero if any harness reported a
fatal (a backend crash / lost cmd / unrecoverable stall).

Usage
-----
    python tests/fuzz/run_smoke.py            # quick smoke (small budgets)
    FUZZ_ITERS=1000 python tests/fuzz/run_smoke.py   # heavier override
    FUZZ_DURATION=20 python tests/fuzz/run_smoke.py  # longer storms

Explicit FUZZ_ITERS / FUZZ_DURATION in the environment override the
per-harness smoke defaults below.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from _common import wait_port_free  # noqa: E402

HERE = Path(__file__).resolve().parent
PY = sys.executable

# name, script, smoke env defaults (overridable by an explicit env var)
HARNESSES = [
    ("ws_cmd",             "harness_ws_cmd.py",            {"FUZZ_ITERS": "60"}),
    ("config",             "harness_config.py",            {"FUZZ_ITERS": "40"}),
    ("emit_trigger",       "harness_emit_trigger.py",      {"FUZZ_ITERS": "60"}),
    ("emit_x_cmd",         "harness_emit_x_cmd.py",        {"FUZZ_DURATION": "4"}),
    ("cmd_during_compile", "harness_cmd_during_compile.py", {"FUZZ_ITERS": "2"}),
    ("set_param_storm",    "harness_set_param_storm.py",   {"FUZZ_DURATION": "4"}),
]


def run_one(name: str, script: str, env_defaults: dict) -> dict:
    print(f"\n{'=' * 64}\n[run_smoke] running '{name}'\n{'=' * 64}", flush=True)
    # Wait for any previous harness's backend to fully release the port —
    # the single-client server 503s an attach to a still-draining backend.
    if not wait_port_free(10.0):
        print(f"[run_smoke] WARN: port {7823} still busy before '{name}'", flush=True)
    env = os.environ.copy()
    for k, v in env_defaults.items():
        env.setdefault(k, v)
    t0 = time.time()
    rc = subprocess.call([PY, str(HERE / script)], cwd=str(HERE), env=env)
    dt = time.time() - t0
    print(f"[run_smoke] '{name}' rc={rc} took={dt:.1f}s", flush=True)

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
    # rc != 0 with no explicit fatal still counts as a failure signal
    if summary.get("fatals", 0) == 0 and rc != 0:
        summary["fatals"] = 1
        summary.setdefault("note", f"nonzero rc={rc}")
    return summary


def main() -> int:
    only = set(sys.argv[1:])  # optional: run only named harnesses
    summary = []
    for name, script, env_defaults in HARNESSES:
        if only and name not in only:
            continue
        summary.append(run_one(name, script, env_defaults))

    print("\n" + "=" * 64)
    print("[run_smoke] SUMMARY")
    print("=" * 64)
    for s in summary:
        print(json.dumps(s))
    out = HERE / "_run_smoke_summary.json"
    out.write_text(json.dumps(summary, indent=2))
    print(f"\nwrote {out}")
    total_fatals = sum(s.get("fatals", 0) for s in summary)
    print(f"[run_smoke] total fatals = {total_fatals}")
    return 0 if total_fatals == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
