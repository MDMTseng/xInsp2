"""Tie the SDK's hand-rolled `RunOutcome` model to the contract-generated
TypedDict for `xi.run-outcome/1`.

The SDK deliberately keeps a *dataclass* `RunOutcome` (its public, versioned
API — property helpers, keyword-safe `verdict_class`, backward-tolerant
parsing) rather than consuming the generated `TypedDict` directly. To stop the
two drifting, this test imports the generated type from
`contract/codegen/generated/run_outcome.generated.py` and asserts field
compatibility: every wire key the schema defines has a home on the SDK model
(with the `class` -> `verdict_class` keyword rename), the required `schema`
literal matches `RUN_RESULT_SCHEMA`, and the `class` enum matches the SDK's
`CLASS_*` constants. Regenerating the type after a schema change will fail this
test if the SDK model wasn't updated to match.

Runs without a live backend.

    pytest tools/xinsp2_py/tests/test_contract_types.py
"""
from __future__ import annotations

import dataclasses
import importlib.util
import typing
from pathlib import Path

import pytest

from xinsp2 import (
    RunOutcome,
    RUN_RESULT_SCHEMA,
    CLASS_OK,
    CLASS_NG,
    CLASS_NA,
    CLASS_NO_VERDICT,
    CLASS_CRASHED,
    CLASS_DROPPED,
)

# contract/ lives at the repo root, four levels up from this file.
GEN = (
    Path(__file__).resolve().parents[3]
    / "contract" / "codegen" / "generated" / "run_outcome.generated.py"
)

# JSON wire key -> SDK dataclass field. Only the `class` keyword needs a rename;
# every other wire key maps to an identically-named field.
_WIRE_TO_FIELD = {"class": "verdict_class"}

# SDK-only fields that are not wire keys (internal bookkeeping).
_SDK_INTERNAL = {"raw"}


@pytest.fixture(scope="module")
def generated():
    if not GEN.exists():
        pytest.skip(f"generated contract type not found at {GEN}")
    spec = importlib.util.spec_from_file_location("_run_outcome_generated", GEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _wire_keys(td) -> set[str]:
    return set(td.__annotations__.keys())


def test_every_wire_key_has_an_sdk_field(generated):
    sdk_fields = {f.name for f in dataclasses.fields(RunOutcome)}
    for key in _wire_keys(generated.RunOutcome):
        field = _WIRE_TO_FIELD.get(key, key)
        assert field in sdk_fields, (
            f"contract wire key {key!r} (SDK field {field!r}) is missing from "
            f"RunOutcome — the SDK model drifted from the generated type"
        )


def test_sdk_declares_no_phantom_wire_fields(generated):
    # Every SDK field (minus internal bookkeeping) must correspond to a real
    # wire key, so the SDK can't grow a field the contract doesn't define.
    wire_fields = {_WIRE_TO_FIELD.get(k, k) for k in _wire_keys(generated.RunOutcome)}
    for f in dataclasses.fields(RunOutcome):
        if f.name in _SDK_INTERNAL:
            continue
        assert f.name in wire_fields, (
            f"RunOutcome field {f.name!r} has no matching contract wire key — "
            f"either the schema is stale or the SDK invented a field"
        )


def test_schema_literal_matches_sdk_constant(generated):
    ann = generated.RunOutcome.__annotations__["schema"]
    # schema is Required[Literal["xi.run-outcome/1"]]; unwrap to the literal.
    literals = _flatten_literals(ann)
    assert RUN_RESULT_SCHEMA in literals
    assert literals == {RUN_RESULT_SCHEMA}


def test_class_enum_matches_sdk_class_constants(generated):
    ann = generated.RunOutcome.__annotations__["class"]
    literals = _flatten_literals(ann)
    assert literals == {
        CLASS_OK, CLASS_NG, CLASS_NA, CLASS_NO_VERDICT, CLASS_CRASHED, CLASS_DROPPED,
    }


def _flatten_literals(ann) -> set:
    """Collect Literal[...] values out of an annotation, peeling any
    Required/NotRequired wrapper."""
    args = typing.get_args(ann)
    # Required[...]/NotRequired[...] wrap a single inner type.
    origin = typing.get_origin(ann)
    if origin in (typing.Literal,):
        return set(args)
    for a in args:
        got = _flatten_literals(a)
        if got:
            return got
    return set()
