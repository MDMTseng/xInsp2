#!/usr/bin/env python3
"""Protocol-version gate — makes the WS protocol version REAL without touching the wire.

The problem this closes (docs/ext_review/06 finding 2): the `hello` event's
`abi` stamp has been the constant `1` across genuine wire breaks (the removed
`vars` message, partial-status returns, ...). It was never bumped and never
enforced, so the *described* shape of the wire could drift silently. The
extension now treats `abi != 1` as incompatible (vscode-extension/src/versionCompat.ts),
so the day the backend bumps the stamp, clients react — but nothing decided WHEN
it must bump. This gate is that decision, mechanised.

How it works. `contract/baseline/protocol-baseline.json` is a committed snapshot
of the canonical *shape* of every text-wire schema under contract/schemas/, plus
the `protocol_version` that snapshot corresponds to (the current `abi`, 1). On
every build this gate recomputes the live shapes and compares them to the
baseline:

  * unchanged           -> green.
  * ADDITIVE change     -> FAIL, demanding the baseline be refreshed in the SAME
                           commit (`--update-baseline`). No version bump: additive
                           evolution is what the additive-only wire already allows.
  * BREAKING change     -> FAIL, demanding BOTH a baseline refresh AND a
                           protocol_version bump (`--update-baseline --protocol-version N`),
                           with a pointer to the coordinated-cutover policy.

So a schema cannot change without the baseline changing with it, and a *breaking*
schema change cannot land without the recorded protocol version moving. The gate
does NOT read or change the live `abi` value the backend sends — bumping that is
the cutover train's move (docs/ext_review/00-triage.md "Two branch sets"). It only
makes it impossible for the described shape to drift out from under the stamp.

Canonical shape (what is compared). Each schema is reduced to its shape-bearing
keywords; documentation-only annotations (title/description/$comment/examples/
default/deprecated/format — the ones the subset marks "not asserted") are
STRIPPED before hashing and diffing. Consequence: editing a description does NOT
trip this gate (it is not a wire change); changing a type, property, enum, or
constraint does.

Pure stdlib (json + hashlib) — no jsonschema dependency, unlike validate.py.
Wired as the `contract_baseline` ctest. Run standalone:

    python contract/baseline_gate.py                     # the gate (read-only)
    python contract/baseline_gate.py --update-baseline   # refresh (additive/unchanged)
    python contract/baseline_gate.py --update-baseline --protocol-version 2   # refresh + bump (breaking)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "contract"
SCHEMA_DIR = CONTRACT / "schemas"
BASELINE_PATH = CONTRACT / "baseline" / "protocol-baseline.json"

REFRESH_CMD = "python contract/baseline_gate.py --update-baseline"
REFRESH_CMD_BUMP = "python contract/baseline_gate.py --update-baseline --protocol-version <N>"
CUTOVER_POINTER = (
    "Breaking wire changes ride the coordinated cutover train, not master: stage "
    "on the integration branch and cut over with the app-dev team in one step "
    "(docs/ext_review/00-triage.md \"Two branch sets\"). Bumping protocol_version "
    "here is the description half; the live `abi` stamp in send_hello "
    "(backend/src/service_cmd_lifecycle.cpp) is bumped in lockstep AT that cutover "
    "- the extension already rejects a mismatched abi (vscode-extension/src/versionCompat.ts)."
)

# Documentation-only annotations: present in the subset purely to document intent,
# never asserted for validation (contract/README.md, meta-schema `format` note).
# Stripped before hashing/diffing so a prose edit is not seen as a wire change.
ANNOTATION_KEYS = frozenset(
    {"title", "description", "$comment", "examples", "default", "deprecated", "format"}
)

# Scalar/array constraint keywords. Any change to one of these is treated as
# BREAKING (conservative): tightening rejects values that used to be valid, and
# even loosening can break a consumer that sized itself to the old bound.
CONSTRAINT_KEYS = (
    "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum", "multipleOf",
    "minLength", "maxLength", "pattern", "minItems", "maxItems",
)


# --------------------------------------------------------------------------- #
# Canonical shape                                                             #
# --------------------------------------------------------------------------- #
def canonical_shape(node):
    """Reduce a schema (sub)node to its shape-bearing form.

    Strips documentation annotations; sorts order-insensitive lists (`required`,
    `enum`) so a reorder is not a diff; recurses into subschemas. The result is a
    stable structure whose canonical-JSON serialisation is what we hash.
    """
    if isinstance(node, dict):
        out = {}
        for k, v in node.items():
            if k in ANNOTATION_KEYS:
                continue
            if k == "required" and isinstance(v, list):
                out[k] = sorted(v)
            elif k == "enum" and isinstance(v, list):
                # Order is semantically irrelevant; sort by canonical repr.
                out[k] = sorted(v, key=lambda x: json.dumps(x, sort_keys=True))
            else:
                out[k] = canonical_shape(v)
        return out
    if isinstance(node, list):
        return [canonical_shape(x) for x in node]
    return node


def canonical_json(obj) -> str:
    return json.dumps(obj, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def sha256_of(shape) -> str:
    return hashlib.sha256(canonical_json(shape).encode("utf-8")).hexdigest()


def load(p: Path):
    return json.loads(p.read_text(encoding="utf-8"))


def live_schemas() -> dict:
    """name -> canonical shape for every schema under contract/schemas/."""
    out = {}
    for sp in sorted(SCHEMA_DIR.glob("*.json")):
        out[sp.name] = canonical_shape(load(sp))
    return out


# --------------------------------------------------------------------------- #
# Classification: additive vs breaking                                        #
# --------------------------------------------------------------------------- #
def _enum_key(v) -> str:
    return json.dumps(v, sort_keys=True)


def diff_node(old, new, path, open_enums, findings):
    """Compare two canonical shape nodes, appending (severity, message) findings.

    severity is "breaking" or "additive". The precise ruleset (also in README):

      BREAKING — a change a consumer built against `old` cannot absorb:
        * property removed
        * property type changed (incl. nullability, e.g. "string" -> ["string","null"])
        * property added as REQUIRED, or an existing property made required
        * an existing required property made optional (presence guarantee dropped)
        * enum value removed (a value producers may still emit becomes invalid)
        * enum value added on a CLOSED enum (one not listed in baseline open_enums)
        * const changed; any scalar/array constraint changed; $ref target changed
        * additionalProperties tightened true -> false
        * new optional property added to a CLOSED object (additionalProperties:false)

      ADDITIVE — tolerated by the additive-only wire:
        * new OPTIONAL property on an object that tolerates unknowns
          (additionalProperties true/absent in the OLD baseline)
        * enum value added on an OPEN enum (path listed in baseline open_enums)
        * additionalProperties loosened false -> true
    """
    if not isinstance(old, dict) or not isinstance(new, dict):
        if old != new:
            findings.append(("breaking", f"{path or '<root>'}: shape changed"))
        return

    if old.get("type") != new.get("type"):
        findings.append(("breaking",
            f"{path or '<root>'}: type changed {old.get('type')!r} -> {new.get('type')!r}"))
    if old.get("const") != new.get("const"):
        findings.append(("breaking",
            f"{path or '<root>'}: const changed {old.get('const')!r} -> {new.get('const')!r}"))
    if old.get("$ref") != new.get("$ref"):
        findings.append(("breaking",
            f"{path or '<root>'}: $ref target changed {old.get('$ref')!r} -> {new.get('$ref')!r}"))

    # enum
    oe, ne = old.get("enum"), new.get("enum")
    if oe is not None or ne is not None:
        if oe is None or ne is None:
            findings.append(("breaking",
                f"{path or '<root>'}: enum constraint {'added' if oe is None else 'removed'}"))
        else:
            oset = {_enum_key(v) for v in oe}
            nset = {_enum_key(v) for v in ne}
            removed = oset - nset
            added = nset - oset
            if removed:
                findings.append(("breaking",
                    f"{path or '<root>'}: enum value(s) removed: {sorted(removed)}"))
            if added:
                if path in open_enums:
                    findings.append(("additive",
                        f"{path or '<root>'}: enum value(s) added on an OPEN enum: {sorted(added)}"))
                else:
                    findings.append(("breaking",
                        f"{path or '<root>'}: enum value(s) added on a CLOSED enum: {sorted(added)} "
                        f"(if consumers tolerate unknown values, declare this path in the "
                        f"baseline's open_enums to make additions additive)"))

    # scalar / array constraints
    for kw in CONSTRAINT_KEYS:
        if old.get(kw) != new.get(kw):
            findings.append(("breaking",
                f"{path or '<root>'}: constraint {kw} changed {old.get(kw)!r} -> {new.get(kw)!r}"))

    # additionalProperties
    oap, nap = old.get("additionalProperties"), new.get("additionalProperties")
    if oap != nap:
        if oap is True and nap is False:
            findings.append(("breaking",
                f"{path or '<root>'}/additionalProperties: tightened true -> false "
                f"(previously-tolerated unknown fields now rejected)"))
        elif oap is False and nap is True:
            findings.append(("additive",
                f"{path or '<root>'}/additionalProperties: loosened false -> true"))
        elif isinstance(oap, dict) and isinstance(nap, dict):
            diff_node(oap, nap, f"{path}/additionalProperties", open_enums, findings)
        else:
            findings.append(("breaking",
                f"{path or '<root>'}/additionalProperties: changed {oap!r} -> {nap!r}"))

    # properties + required transitions
    op = old.get("properties", {}) or {}
    npr = new.get("properties", {}) or {}
    oreq = set(old.get("required", []) or [])
    nreq = set(new.get("required", []) or [])
    old_tolerant = old.get("additionalProperties", True) is True
    for name in npr:
        cp = f"{path}/properties/{name}"
        if name not in op:
            if name in nreq:
                findings.append(("breaking", f"{cp}: new REQUIRED property"))
            elif old_tolerant:
                findings.append(("additive", f"{cp}: new optional property"))
            else:
                findings.append(("breaking",
                    f"{cp}: new optional property on a closed object "
                    f"(additionalProperties:false) — old consumers reject unknown keys"))
        else:
            was_req, is_req = name in oreq, name in nreq
            if not was_req and is_req:
                findings.append(("breaking", f"{cp}: property became REQUIRED"))
            elif was_req and not is_req:
                findings.append(("breaking",
                    f"{cp}: required property made optional (presence guarantee removed)"))
            diff_node(op[name], npr[name], cp, open_enums, findings)
    for name in op:
        if name not in npr:
            findings.append(("breaking", f"{path}/properties/{name}: property REMOVED"))

    # array items
    oi, ni = old.get("items"), new.get("items")
    if isinstance(oi, dict) and isinstance(ni, dict):
        diff_node(oi, ni, f"{path}/items", open_enums, findings)
    elif (oi is None) != (ni is None):
        findings.append(("breaking",
            f"{path}/items: items schema {'added' if oi is None else 'removed'}"))

    # $defs
    od = old.get("$defs", {}) or {}
    nd = new.get("$defs", {}) or {}
    for k in nd:
        if k in od:
            diff_node(od[k], nd[k], f"{path}/$defs/{k}", open_enums, findings)
    for k in od:
        if k not in nd:
            findings.append(("breaking", f"{path}/$defs/{k}: $def REMOVED"))


def classify(baseline: dict, live: dict):
    """Return (verdict, findings) where verdict is 'unchanged'|'additive'|'breaking'.

    findings is a list of (schema_name, severity, message).
    """
    base_schemas = baseline.get("schemas", {})
    open_enums_map = baseline.get("open_enums", {})
    findings = []

    for name in sorted(set(base_schemas) | set(live)):
        b = base_schemas.get(name)
        l = live.get(name)
        if b is None:
            # New schema file: a new message shape. The discriminator ignores an
            # unknown message type, so this is additive (still needs a refresh).
            findings.append((name, "additive", "new schema (new message shape)"))
            continue
        if l is None:
            findings.append((name, "breaking", "schema REMOVED (a message shape retired)"))
            continue
        if b.get("sha256") == sha256_of(l):
            continue  # unchanged
        open_enums = set(open_enums_map.get(name, []))
        local = []
        diff_node(b.get("shape", {}), l, "", open_enums, local)
        if not local:
            # sha differs but nothing shape-bearing diffed (e.g. a bare $id edit):
            # non-breaking, but the baseline must still be refreshed.
            local = [("additive", "shape digest changed with no structural diff (refresh baseline)")]
        for sev, msg in local:
            findings.append((name, sev, msg))

    if any(sev == "breaking" for _, sev, _ in findings):
        verdict = "breaking"
    elif findings:
        verdict = "additive"
    else:
        verdict = "unchanged"
    return verdict, findings


# --------------------------------------------------------------------------- #
# Baseline read / write                                                       #
# --------------------------------------------------------------------------- #
def build_baseline(live: dict, protocol_version: int, open_enums: dict) -> dict:
    return {
        "$comment": (
            "GENERATED by contract/baseline_gate.py --update-baseline. The committed "
            "snapshot of the canonical WS text-wire shape and the protocol_version it "
            "corresponds to. Do NOT hand-edit the `schemas` block; edit a schema and "
            "run the refresh. `open_enums` is hand-curated policy (preserved across "
            "refreshes) — see contract/README.md."
        ),
        "protocol_version": protocol_version,
        "open_enums": open_enums,
        "schemas": {
            name: {"sha256": sha256_of(shape), "shape": shape}
            for name, shape in sorted(live.items())
        },
    }


def write_baseline(doc: dict) -> None:
    BASELINE_PATH.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(doc, indent=2, ensure_ascii=False, sort_keys=True) + "\n"
    BASELINE_PATH.write_text(text, encoding="utf-8", newline="\n")


# Seed policy for a first-time baseline: the run-outcome `class` enum is documented
# open (reference consumers pass an unknown class through as a literal — see the
# schema's own `class` description), so adding a class value is additive, not breaking.
DEFAULT_OPEN_ENUMS = {"run-outcome.schema.json": ["/properties/class"]}


# --------------------------------------------------------------------------- #
# CLI                                                                         #
# --------------------------------------------------------------------------- #
def run_gate() -> int:
    live = live_schemas()
    if not BASELINE_PATH.exists():
        print("baseline-gate: FAIL - no baseline committed at "
              f"{BASELINE_PATH.relative_to(ROOT).as_posix()}")
        print(f"  create it with: {REFRESH_CMD}")
        return 1
    baseline = load(BASELINE_PATH)
    verdict, findings = classify(baseline, live)
    pv = baseline.get("protocol_version")
    print(f"baseline-gate: {len(live)} schemas, protocol_version={pv}, verdict={verdict}")

    if verdict == "unchanged":
        print("baseline-gate: OK - live wire shape matches the committed baseline")
        return 0

    for name, sev, msg in findings:
        print(f"  - [{sev}] {name}: {msg}")

    if verdict == "additive":
        print("baseline-gate: FAIL - ADDITIVE change: the live wire shape moved but the "
              "baseline was not refreshed in this commit.")
        print(f"  Refresh it (no version bump needed): {REFRESH_CMD}")
        return 1

    print("baseline-gate: FAIL - BREAKING change: the live wire shape moved in a way "
          "consumers cannot absorb.")
    print(f"  Refresh the baseline AND bump protocol_version: {REFRESH_CMD_BUMP}")
    print(f"    (protocol_version must increase from {pv}.)")
    print(f"  {CUTOVER_POINTER}")
    return 1


def run_update(new_version) -> int:
    live = live_schemas()
    old = load(BASELINE_PATH) if BASELINE_PATH.exists() else None
    old_version = old.get("protocol_version") if old else None
    open_enums = old.get("open_enums") if old else None
    if open_enums is None:
        open_enums = DEFAULT_OPEN_ENUMS

    if old is None:
        version = new_version if new_version is not None else 1
        write_baseline(build_baseline(live, version, open_enums))
        print(f"baseline-gate: created baseline at protocol_version={version} "
              f"({len(live)} schemas).")
        return 0

    verdict, findings = classify(old, live)
    for name, sev, msg in findings:
        print(f"  - [{sev}] {name}: {msg}")

    if verdict == "unchanged":
        print("baseline-gate: nothing to refresh - live shape already matches the baseline.")
        return 0

    if verdict == "breaking":
        if new_version is None or new_version <= old_version:
            print("baseline-gate: REFUSING to refresh - this is a BREAKING wire change.")
            print(f"  A breaking change REQUIRES a protocol_version bump above {old_version}.")
            print(f"  Re-run with an explicit bump, e.g.: "
                  f"python contract/baseline_gate.py --update-baseline "
                  f"--protocol-version {old_version + 1}")
            print(f"  {CUTOVER_POINTER}")
            return 1
        write_baseline(build_baseline(live, new_version, open_enums))
        print(f"baseline-gate: BREAKING change recorded - baseline refreshed and "
              f"protocol_version bumped {old_version} -> {new_version}.")
        print(f"  {CUTOVER_POINTER}")
        return 0

    # additive
    if new_version is not None and new_version != old_version:
        print(f"baseline-gate: note - change is ADDITIVE; keeping protocol_version="
              f"{old_version} (a bump is not required for additive changes). "
              f"Ignoring --protocol-version {new_version}.")
    write_baseline(build_baseline(live, old_version, open_enums))
    print(f"baseline-gate: ADDITIVE change recorded - baseline refreshed at "
          f"protocol_version={old_version} (no bump needed).")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="WS protocol-version baseline gate.")
    ap.add_argument("--update-baseline", action="store_true",
                    help="Regenerate the committed baseline from the live schemas.")
    ap.add_argument("--protocol-version", type=int, default=None,
                    help="New protocol_version to record (REQUIRED when refreshing a "
                         "breaking change; ignored for additive/unchanged).")
    args = ap.parse_args()
    if args.update_baseline:
        return run_update(args.protocol_version)
    return run_gate()


if __name__ == "__main__":
    raise SystemExit(main())
