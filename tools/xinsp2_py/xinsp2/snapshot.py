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
    report_path = out / "report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return RunSnapshot(folder=out, report_path=report_path)
