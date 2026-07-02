"""Parse-check the run-outcome wire contract (schema xi.run-outcome/1) and the
lifecycle-status semantics against the protocol golden fixtures.

Runs without a live backend: it loads the JSON fixtures under
`protocol/fixtures/` and drives the pure parsing/classification code paths.

    pytest tools/xinsp2_py/tests
"""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from xinsp2 import (
    RunResult,
    RunOutcome,
    RunFinished,
    outcome_class_for_code,
    RUN_RESULT_SCHEMA,
    XI_SYS_CRASHED,
    XI_SYS_NO_VERDICT,
    CLASS_OK,
    CLASS_CRASHED,
    CLASS_NO_VERDICT,
    CLASS_NA,
    CLASS_NG,
    Client,
    ProtocolError,
    PartialStatusError,
)

# protocol/fixtures lives at the repo root, four levels up from this file:
# tools/xinsp2_py/tests/test_run_outcome.py -> repo root
FIX = Path(__file__).resolve().parents[3] / "protocol" / "fixtures"


def load(name: str) -> dict:
    return json.loads((FIX / name).read_text(encoding="utf-8"))


def test_run_result_ok_exposes_additive_fields():
    ev = load("run_result.json")
    o = RunOutcome.from_event(ev)
    assert o.code == 1
    assert o.run_id == 17
    assert o.schema == RUN_RESULT_SCHEMA
    # `class` JSON key is exposed as verdict_class (keyword-safe).
    assert o.verdict_class == CLASS_OK
    assert o.cls == CLASS_OK
    assert o.is_ok and not o.is_ng and not o.is_crashed
    assert o.trigger_id == "9f3c1a2b4d5e6f708192a3b4c5d6e7f8"
    assert o.boot_id
    assert o.station_id == "cell-A"
    assert o.inspection_id.endswith("/17")
    assert o.reason_code == ""
    assert o.script_generation == 4


def test_run_result_crashed_code_and_class():
    o = RunOutcome.from_event(load("run_result_crashed.json"))
    assert o.code == XI_SYS_CRASHED == -999002
    assert o.verdict_class == CLASS_CRASHED
    assert o.is_crashed and not o.is_ok
    assert o.reason_code == "seh_access_violation"


def test_run_result_no_verdict_code_and_class():
    o = RunOutcome.from_event(load("run_result_no_verdict.json"))
    assert o.code == XI_SYS_NO_VERDICT == -999005
    assert o.is_no_verdict
    assert o.verdict_class == CLASS_NO_VERDICT


def test_outcome_class_for_code_derivation():
    # Distinguish crash / no-verdict / na / ng / ok on the numeric channel.
    assert outcome_class_for_code(1) == CLASS_OK
    assert outcome_class_for_code(0) == CLASS_NA
    assert outcome_class_for_code(-5) == CLASS_NG
    assert outcome_class_for_code(XI_SYS_CRASHED) == CLASS_CRASHED
    assert outcome_class_for_code(XI_SYS_NO_VERDICT) == CLASS_NO_VERDICT


def test_missing_additive_fields_default_to_none():
    # Backward tolerance: an old-style run_result with only the legacy fields.
    o = RunOutcome.from_event({"name": "run_result",
                               "data": {"code": 0, "msg": "", "run_id": 1}})
    assert o.schema is None
    assert o.trigger_id is None
    assert o.script_generation is None
    # class derived from code when absent.
    assert o.cls == CLASS_NA


def test_run_finished_inspect_compute_us():
    f = RunFinished.from_event(load("run_finished.json"))
    assert f.ms == 12
    assert f.inspect_compute_us == 11840


def test_run_result_typed_accessors_on_runresult():
    ro = load("run_result.json")
    rf = load("run_finished.json")
    rr = RunResult(run_id=17, ms=12, data={"run_id": 17, "ms": 12},
                   events=[ro, rf])
    assert rr.outcome is not None and rr.outcome.is_ok
    assert rr.finished is not None and rr.finished.inspect_compute_us == 11840


def test_metrics_rename_tolerant_accessor():
    snap = load("metrics_snapshot.json")["data"]
    block = Client.metrics_inspect_compute(None, snap)  # unbound; snapshot given
    assert block["count"] == 128
    assert "mean_ms" in block and "buckets" in block
    # Legacy key still resolves.
    legacy = {"latency_ms": {"count": 3}}
    assert Client.metrics_inspect_compute(None, legacy)["count"] == 3


def test_load_project_partial_is_not_success():
    fx = load("load_project_partial.json")
    assert fx["ok"] is False
    assert fx["data"]["status"] == "partial"
    # The client turns the backend ok:false into a typed PartialStatusError.
    err = PartialStatusError("load_project", fx["data"]["status"],
                             error=fx.get("error"), data=fx["data"])
    assert err.status == "partial"
    assert isinstance(err, ProtocolError)


def test_commit_group_partial_is_not_success():
    fx = load("commit_group_partial.json")
    assert fx["data"]["status"] == "partial"
    committed = load("commit_group_committed.json")
    assert committed["data"]["status"] == "committed"
