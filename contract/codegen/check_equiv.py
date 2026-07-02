#!/usr/bin/env python3
"""codegen_equiv -- the contract-codegen equivalence + drift gate.

This is the wave-3 exit criterion (docs/new_gen/08 Wave 3, docs/new_gen/02
guard 4): PROVE the generated <plugin>_keys.gen.h is a drop-in for the
hand-written <plugin>_keys.h, and that neither can silently drift from the
decl. Three checks, all pure stdlib (no jsonschema), so it runs wherever a
Python interpreter is present -- like the contract_baseline gate.

  (a) DETERMINISM / not-stale: regenerate every artifact into a tmp dir and
      byte-compare against what is committed in generated/plugins/. A drifted
      commit fails ("regenerate").
  (c) DRIFT (the equivalence core): for each decl, extract the key-constant map
      {kName -> "value"} + kSchemaVersion from the generated _keys.gen.h and from
      the decl itself; they must agree exactly (the generator must emit precisely
      the decl's key set). If a hand-written plugins/<p>/<p>_keys.h is STILL
      present (a plugin declared but not yet swapped), it is ALSO compared so the
      pending swap is proven a true drop-in. Post-swap (docs/new_gen/08 Wave 3)
      that hand-written header is deleted and the plugin includes _keys.gen.h
      directly, so the decl<->generated leg is the whole proof and an absent
      hand-written header is the swapped state, not a failure.
  (e) COVERAGE RATCHET (WARN, non-fatal): list every plugin under plugins/ that
      ships a hand-written *_keys.h but has no decl in contract/plugins/. This
      makes the ratchet direction visible without blocking -- the covered-plugin
      set only grows.

The compiled half of the proof (a TU that includes the GENERATED _keys.gen.h +
_io.gen.h against the same call sites the plugin tests use, proving the API is a
drop-in at compile+run time) is the separate codegen_equiv_test executable
(contract/codegen/equiv/test_codegen_equiv.cpp), wired alongside this in
backend/CMakeLists.txt.

    python contract/codegen/check_equiv.py     # the gate (read-only)
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

import gen_contract as gc  # sibling module

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
PLUGINS = REPO / "plugins"
COMMITTED = HERE / "generated" / "plugins"

# inline constexpr const char* kName = "value";
_KEY_RE = re.compile(r'constexpr\s+const\s+char\*\s+(\w+)\s*=\s*"((?:[^"\\]|\\.)*)"\s*;')
# inline constexpr int kSchemaVersion = N;
_VER_RE = re.compile(r'constexpr\s+int\s+kSchemaVersion\s*=\s*(\d+)\s*;')


def parse_keys_header(text: str) -> tuple[dict[str, str], int | None]:
    consts = {m.group(1): m.group(2) for m in _KEY_RE.finditer(text)}
    ver = _VER_RE.search(text)
    return consts, (int(ver.group(1)) if ver else None)


def decl_key_map(decl: dict) -> dict[str, str]:
    return {gc.k_name(k): k for k, _ in gc.collect_keys(decl)}


def check_determinism() -> list[str]:
    import tempfile
    problems = []
    # The exact filenames the generator produces -- the only names we compare, so
    # stray non-artifacts (e.g. a __pycache__ from importing the generated .py)
    # are ignored.
    artifact_suffixes = tuple(gc.ARTIFACTS.keys())
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        gc.generate(tmp)
        fresh = {p.name for p in tmp.iterdir() if p.is_file()}
        committed = set()
        if COMMITTED.exists():
            committed = {p.name for p in COMMITTED.iterdir()
                         if p.is_file() and p.name.endswith(artifact_suffixes)}
        for name in sorted(fresh):
            c = COMMITTED / name
            if not c.exists():
                problems.append(f"[stale] {name}: not committed under {COMMITTED.relative_to(REPO)}")
            elif c.read_text(encoding="utf-8") != (tmp / name).read_text(encoding="utf-8"):
                problems.append(f"[stale] {name}: committed copy differs from regenerated")
        for name in sorted(committed - fresh):
            problems.append(f"[stale] {name}: committed but no decl produces it")
    return problems


def check_drift(decl: dict) -> list[str]:
    plugin = decl["plugin"]
    problems = []
    gen_h = COMMITTED / f"{plugin}_keys.gen.h"
    hand_h = PLUGINS / plugin / f"{plugin}_keys.h"
    gen_map, gen_ver = parse_keys_header(gen_h.read_text(encoding="utf-8")) if gen_h.exists() else ({}, None)
    dm = decl_key_map(decl)

    # decl <-> generated (the generator must emit exactly the decl's key set)
    if set(gen_map.items()) != set(dm.items()):
        problems.append(f"{plugin}: generated _keys.gen.h != decl key set "
                        f"(gen-only={sorted(set(gen_map.items()) - set(dm.items()))}, "
                        f"decl-only={sorted(set(dm.items()) - set(gen_map.items()))})")
    if gen_ver != decl["schema_version"]:
        problems.append(f"{plugin}: generated kSchemaVersion {gen_ver} != decl {decl['schema_version']}")

    # decl <-> hand-written (OPPORTUNISTIC). Post-swap (docs/new_gen/08 Wave 3)
    # the plugin has DELETED its hand-written plugins/<p>/<p>_keys.h and consumes
    # the generated _keys.gen.h directly, so the decl is the sole source of truth
    # and the decl<->generated leg above is the whole drop-in proof. While a plugin
    # is still pre-swap (a hand-written keys.h present alongside a fresh decl), this
    # leg keeps guarding that the two agree so the swap will be a true drop-in. An
    # ABSENT hand-written header therefore means "swapped", not "unproven".
    if hand_h.exists():
        hand_map, hand_ver = parse_keys_header(hand_h.read_text(encoding="utf-8"))
        if set(hand_map.items()) != set(dm.items()):
            problems.append(f"{plugin}: hand-written {hand_h.relative_to(REPO)} has DRIFTED from its decl "
                            f"(hand-only={sorted(set(hand_map.items()) - set(dm.items()))}, "
                            f"decl-only={sorted(set(dm.items()) - set(hand_map.items()))})")
        if hand_ver != decl["schema_version"]:
            problems.append(f"{plugin}: hand-written kSchemaVersion {hand_ver} != decl {decl['schema_version']}")
    return problems


def coverage_warn(decls: list[dict]) -> list[str]:
    have = {d["plugin"] for d in decls}
    missing = []
    for keys_h in sorted(PLUGINS.glob("*/*_keys.h")):
        plugin = keys_h.parent.name
        if plugin not in have:
            missing.append(plugin)
    return missing


def main() -> int:
    decls = gc.load_decls()
    if not decls:
        print("codegen_equiv: no decls under contract/plugins/ -- nothing to prove")
        return 0

    problems: list[str] = []
    problems += check_determinism()
    for decl in decls:
        problems += check_drift(decl)

    print(f"codegen_equiv: {len(decls)} decl(s): {', '.join(d['plugin'] for d in decls)}")

    missing = coverage_warn(decls)
    if missing:
        print(f"codegen_equiv: WARNING -- {len(missing)} plugin(s) ship a hand-written "
              f"*_keys.h with NO decl (coverage ratchet, non-fatal):")
        for p in missing:
            print(f"    - {p}  (add contract/plugins/{p}.decl.json to bring it under codegen)")

    if problems:
        print(f"\ncodegen_equiv: FAIL -- {len(problems)} problem(s):")
        for p in problems:
            print(f"    - {p}")
        return 1

    print("codegen_equiv: PASS -- generated headers are a drop-in; decls and generated "
          "key sets agree (and any still-present hand-written header agrees too).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
