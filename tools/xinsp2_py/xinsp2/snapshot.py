"""Dump a `RunResult` to disk as a self-contained snapshot folder.

Layout (one folder per run):

    <out>/run-000017/
        report.json        — run_id, ms, verdict, raw run data, event log

This client is generic: it does not collect VARs or image previews, so a
snapshot is just the run's outcome metadata. The owning plugin's webUI is
responsible for any image/preview artifacts.

The AI agent can then `Read` `report.json` like any other source artifact.
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from .client import RunResult


@dataclass
class RunSnapshot:
    folder: Path
    report_path: Path


def dump_run(run: RunResult, out_dir: str | Path, *, prefix: str = "run") -> RunSnapshot:
    out = Path(out_dir) / f"{prefix}-{run.run_id:06d}"
    out.mkdir(parents=True, exist_ok=True)

    report = {
        "run_id": run.run_id,
        "ms": run.ms,
        "verdict": run.verdict,
        "data": run.data,
        "events": run.events,
    }
    # Fold the typed per-run outcome/timing into the report when the backend
    # emitted the events, so a snapshot records the full run-outcome contract
    # (class, identity, reason_code, inspect_compute_us) not just the raw data.
    outcome = run.outcome
    if outcome is not None:
        report["outcome"] = {
            "code": outcome.code,
            "class": outcome.verdict_class,
            "reason_code": outcome.reason_code,
            "trigger_id": outcome.trigger_id,
            "boot_id": outcome.boot_id,
            "station_id": outcome.station_id,
            "inspection_id": outcome.inspection_id,
            "schema": outcome.schema,
            "script_generation": outcome.script_generation,
        }
    finished = run.finished
    if finished is not None:
        report["inspect_compute_us"] = finished.inspect_compute_us
    report_path = out / "report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return RunSnapshot(folder=out, report_path=report_path)
