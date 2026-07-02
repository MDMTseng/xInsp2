#!/usr/bin/env python3
"""Contract gate — validates the xInsp wire contract BOTH ways.

Way A (subset conformance): every schema under contract/schemas/ must itself be
  (1) a valid JSON Schema (draft 2020-12), and
  (2) inside the constrained subset — i.e. it must validate against the
      meta-schema contract/meta/xi-contract-subset.schema.json. A schema that
      reaches for a banned keyword (anyOf/oneOf/allOf/not, if/then/else,
      patternProperties, ...) fails here.

Way B (fixture conformance): every wire fixture under the scan dirs listed in
  contract/fixtures-map.json must validate against its mapped schema. The map is
  the discriminator (the subset has no oneOf, so message routing lives in the
  harness). Strict: a fixture with no mapping is a FAILURE, so a new fixture
  cannot land unvalidated.

Exit code 0 = clean gate; non-zero = at least one failure. Pure stdlib + the
`jsonschema` package (already a repo QA dependency; used by tools/run_qa.py's
environment). Wired as a ctest (see backend/CMakeLists.txt) so green is enforced,
not merely claimed. Run standalone:

    python contract/validate.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

try:
    from jsonschema import Draft202012Validator
    from jsonschema.exceptions import best_match
    from referencing import Registry, Resource
    from referencing.jsonschema import DRAFT202012
except ImportError as e:  # pragma: no cover - environment guard
    print(f"contract-validate: SKIP - jsonschema/referencing not available ({e})")
    # Skip (not fail) so a Python-without-jsonschema box still configures/builds,
    # mirroring the doc_coverage ctest's no-interpreter skip.
    sys.exit(0)

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "contract"
SCHEMA_DIR = CONTRACT / "schemas"
META_PATH = CONTRACT / "meta" / "xi-contract-subset.schema.json"
MAP_PATH = CONTRACT / "fixtures-map.json"


def load(p: Path) -> dict:
    return json.loads(p.read_text(encoding="utf-8"))


def main() -> int:
    errors: list[str] = []
    notes: list[str] = []

    meta = load(META_PATH)
    meta_validator = Draft202012Validator(meta)

    # Load every schema; build a $id -> resource registry for cross-file $ref.
    schemas: dict[str, dict] = {}
    resources: list[tuple[str, Resource]] = []
    for sp in sorted(SCHEMA_DIR.glob("*.json")):
        doc = load(sp)
        schemas[sp.name] = doc
        sid = doc.get("$id")
        if not sid:
            errors.append(f"[schema] {sp.name}: missing $id (needed for $ref resolution)")
            continue
        resources.append((sid, Resource(contents=doc, specification=DRAFT202012)))
    registry = Registry().with_resources(resources)

    # --- Way A: subset conformance -----------------------------------------
    for name, doc in sorted(schemas.items()):
        # A1: valid JSON Schema at all?
        try:
            Draft202012Validator.check_schema(doc)
        except Exception as e:  # noqa: BLE001 - report any schema-validity failure
            errors.append(f"[subset] {name}: not a valid draft-2020-12 schema: {e}")
            continue
        # A2: inside the constrained subset (validates against the meta-schema)?
        merr = best_match(meta_validator.iter_errors(doc))
        if merr is not None:
            loc = "/".join(str(x) for x in merr.absolute_path) or "<root>"
            errors.append(
                f"[subset] {name}: uses a keyword outside the allowed subset at "
                f"'{loc}': {merr.message}"
            )

    # --- Way B: fixture conformance ----------------------------------------
    fmap = load(MAP_PATH)
    mapping: dict[str, str] = fmap["map"]
    scan_dirs: list[str] = fmap["scan_dirs"]

    # Strict coverage: every fixture in a scan dir must be mapped.
    found: list[str] = []
    for d in scan_dirs:
        for fp in sorted((ROOT / d).glob("*.json")):
            rel = fp.relative_to(ROOT).as_posix()
            found.append(rel)
            if rel not in mapping:
                errors.append(
                    f"[coverage] {rel}: fixture is not in contract/fixtures-map.json "
                    f"(strict gate - map it to a schema or remove it)"
                )

    # Every mapped fixture must exist and validate.
    for rel, schema_name in sorted(mapping.items()):
        fp = ROOT / rel
        if not fp.exists():
            errors.append(f"[map] {rel}: mapped fixture does not exist")
            continue
        if schema_name not in schemas:
            errors.append(f"[map] {rel}: mapped schema '{schema_name}' not found in contract/schemas/")
            continue
        instance = load(fp)
        validator = Draft202012Validator(schemas[schema_name], registry=registry)
        verr = best_match(validator.iter_errors(instance))
        if verr is not None:
            loc = "/".join(str(x) for x in verr.absolute_path) or "<root>"
            errors.append(
                f"[fixture] {rel} vs {schema_name}: FAILS at '{loc}': {verr.message}"
            )

    # Orphans are reported, not failed (the map already routes them).
    for rel, why in fmap.get("orphans", {}).items():
        notes.append(f"orphan fixture {rel}: {why}")

    # --- Report ------------------------------------------------------------
    print(f"contract-validate: {len(schemas)} schemas, {len(found)} fixtures scanned")
    for n in notes:
        print(f"  note: {n}")
    if errors:
        print(f"contract-validate: FAIL ({len(errors)} problem(s)):")
        for e in errors:
            print(f"  - {e}")
        return 1
    print("contract-validate: OK - subset conformance + fixture conformance both green")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
