"""Dump a `RunResult` to disk as a self-contained snapshot folder.

Layout (one folder per run):

    <out>/run-000017/
        report.json                    — run_id, ms, per-channel values + image
                                         manifest, event log
        <channel>/values.json          — the channel's scalar values dict
        <channel>/<key>.jpg            — each exposed image (always JPEG)

The AI agent can then `Read` these files like any other source artifact.

Output is organised by `expose` channel (string id) and image key, matching
the post-v9 atomic XEX1 frame model (the old `vars`/`gid`/codec model is gone;
`expose` images are always JPEG).
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from pathlib import Path

from .client import RunResult


@dataclass
class RunSnapshot:
    folder: Path
    report_path: Path


def _safe(name: str) -> str:
    """Make a channel/key id safe to use as a folder/file name."""
    return re.sub(r"[^A-Za-z0-9._-]", "_", name) or "_"


def dump_run(run: RunResult, out_dir: str | Path, *, prefix: str = "run") -> RunSnapshot:
    out = Path(out_dir) / f"{prefix}-{run.run_id:06d}"
    out.mkdir(parents=True, exist_ok=True)

    report_channels = {}
    for channel, frame in run.frames.items():
        ch_dir = out / _safe(channel)
        ch_dir.mkdir(parents=True, exist_ok=True)

        # scalar values
        values_path = ch_dir / "values.json"
        values_path.write_text(json.dumps(frame.values, indent=2), encoding="utf-8")

        # images (always JPEG)
        images = []
        for key, jpeg in frame.images.items():
            img_path = ch_dir / f"{_safe(key)}.jpg"
            img_path.write_bytes(jpeg)
            images.append({
                "key": key,
                "bytes": len(jpeg),
                "file": str(img_path.relative_to(out)),
            })

        report_channels[channel] = {
            "seq": frame.seq,
            "values": frame.values,
            "values_file": str(values_path.relative_to(out)),
            "images": images,
        }

    report = {
        "run_id": run.run_id,
        "ms": run.ms,
        "channels": report_channels,
        "events": run.events,
    }
    report_path = out / "report.json"
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return RunSnapshot(folder=out, report_path=report_path)
