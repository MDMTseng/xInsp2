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


# A minimal duck-typed stand-in for Client so we can drive the real
# client.commit_group / client.load_project mapping code (which calls self.call)
# against a fixture WITHOUT a live backend: `call` replays exactly what the
# backend would return/raise for that fixture. This couples the assertions to the
# CLIENT'S handling of the backend shape, not to a fixture's literal fields.
class _ReplayClient:
    def __init__(self, fixture: dict):
        self._fx = fixture

    def call(self, name, args=None, timeout=None):
        # Mirror Client.call: on ok:false raise ProtocolError carrying data; on
        # ok:true return rsp["data"] (see client.Client.call).
        if not self._fx.get("ok"):
            raise ProtocolError(
                f"cmd {name!r} failed: {self._fx.get('error')}",
                error=self._fx.get("error"),
                data=self._fx.get("data"),
            )
        return self._fx.get("data")


def test_metrics_snapshot_matches_backend_shape():
    snap = load("metrics_snapshot.json")["data"]
    block = Client.metrics_inspect_compute(None, snap)  # unbound; snapshot given
    assert block["count"] == 128
    assert "mean_ms" in block and "buckets" in block
    # buckets is an ARRAY of {le,count} (MetricsRegistry::snapshot_json), NOT a
    # fixed-key le_1..gt_50 object — assert the real shape so a regression to the
    # old fixture shape fails here.
    assert isinstance(block["buckets"], list)
    assert all("le" in b and "count" in b for b in block["buckets"])
    # The final entry is the "inf" overflow bucket (le is the string "inf").
    assert block["buckets"][-1]["le"] == "inf"
    # Top-level frame counters exist (the old fixture omitted them) and partition.
    assert snap["frames_total"] == snap["frames_ok"] + snap["frames_error"]
    assert sum(b["count"] for b in block["buckets"]) == snap["frames_total"]
    # Legacy key still resolves via the rename-tolerant accessor.
    legacy = {"latency_ms": {"count": 3}}
    assert Client.metrics_inspect_compute(None, legacy)["count"] == 3


def test_load_project_ok_returns_status_with_empty_warning_arrays():
    fx = load("load_project_ok.json")
    assert fx["ok"] is True
    # Backend ALWAYS emits the warning arrays (empty on a clean load), not omitted.
    assert fx["data"]["param_warnings"] == []
    assert fx["data"]["instance_warnings"] == []
    out = Client.load_project(_ReplayClient(fx), "recipe.json")  # unbound self
    assert out["status"] == "ok"


def test_load_project_partial_raises_partial_status_error():
    fx = load("load_project_partial.json")
    # Backend truth: a partial restore is ok:false, so the client re-raises the
    # ProtocolError as a typed PartialStatusError carrying .status.
    assert fx["ok"] is False and fx["data"]["status"] == "partial"
    with pytest.raises(PartialStatusError) as ei:
        Client.load_project(_ReplayClient(fx), "recipe.json")
    assert ei.value.status == "partial"
    assert isinstance(ei.value, ProtocolError)


def test_commit_group_committed_returns_results_shape():
    fx = load("commit_group_committed.json")
    assert fx["ok"] is True and fx["data"]["status"] == "committed"
    out = Client.commit_group(_ReplayClient(fx), group="main")  # unbound self
    assert out["status"] == "committed"
    # Backend truth: data.results[] with per-target {name, ok, result} — NOT the
    # old {committed, canonical, warnings} shape. Assert both so a regression fails.
    assert isinstance(out["results"], list) and out["results"]
    for r in out["results"]:
        assert "name" in r and "ok" in r
    assert "committed" not in out and "canonical" not in out


def test_commit_group_partial_raises_partial_status_error():
    fx = load("commit_group_partial.json")
    # Backend truth: a partial commit is ok:false (dispatch halted, config fault
    # latched); the client must re-raise as PartialStatusError, not leak a bare
    # ProtocolError. This is the case the client previously mishandled because the
    # old fixture wrongly pinned ok:true.
    assert fx["ok"] is False and fx["data"]["status"] == "partial"
    with pytest.raises(PartialStatusError) as ei:
        Client.commit_group(_ReplayClient(fx), group="main")
    assert ei.value.status == "partial"


def test_partial_status_implies_ok_false_invariant():
    # The constrained contract subset cannot express "status==partial => ok==false"
    # (no if/then — schema-spike finding). Guard the pairing here so it holds for
    # every partial fixture, and pairs the other way for the success fixtures.
    for name in ("load_project_partial.json", "commit_group_partial.json"):
        fx = load(name)
        assert fx["data"]["status"] == "partial"
        assert fx["ok"] is False, f"{name}: status==partial must imply ok==false"
    assert load("commit_group_committed.json")["ok"] is True
    assert load("load_project_ok.json")["ok"] is True
