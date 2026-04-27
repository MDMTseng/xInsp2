"""Dump a `RunResult` to disk as a self-contained snapshot folder.

Layout (one folder per run):

    <out>/run-000017/
        report.json        — run_id, ms, vars (without binary), event log
        vars/<name>.<ext>  — image previews decoded by codec extension
        vars/<name>.json   — non-image VARs as one file each (for grep-ability)

The AI agent can then `Read` these files like any other source artifact.
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
    vars_dir = out / "vars"
    vars_dir.mkdir(parents=True, exist_ok=True)

    report_vars = []
    for item in run.vars:
        kind = item["kind"]
        name = item["name"]
        if kind == "image":
            pf = run.previews.get(item["gid"])
            if pf is None:
                report_vars.append({**item, "missing_preview": True})
                continue
            ext = {0: "jpg", 1: "bmp", 2: "png"}.get(pf.codec, "bin")
            img_path = vars_dir / f"{name}.{ext}"
            img_path.write_bytes(pf.payload)
            report_vars.append({
                "name": name, "kind": "image",
                "width": pf.width, "height": pf.height, "channels": pf.channels,
                "codec": pf.codec_name, "file": str(img_path.relative_to(out)),
            })
        else:
            report_vars.append({k: v for k, v in item.items() if k != "gid"})
            if kind == "json":
                (vars_dir / f"{name}.json").write_text(
                    json.dumps(item.get("value"), indent=2), encoding="utf-8")

    report = {
        "run_id": run.run_id,
        "ms": run.ms,
        "vars": report_vars,
        "events": run.events,
    }
    report_path = out / "report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return RunSnapshot(folder=out, report_path=report_path)
