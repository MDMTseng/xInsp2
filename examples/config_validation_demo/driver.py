"""config_validation_demo — exercises FL r6 P2-3.

Opens this project and prints `open_project_warnings`. The
`inst_typo` instance.json deliberately contains:

    - "fsp"   (unknown_config_key — should be "fps")
    - "shape": "burts"  (not_in_enum — must be steady/bursty/variable)
    - "width": 10000     (out_of_range — manifest says max 1920)
    - "height": "tall"   (type_mismatch — manifest says int)

Expected output: four warning entries on `inst_typo`, zero on
`inst_clean`. The project still loads — warnings are non-fatal.
"""
from __future__ import annotations
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# Make the in-tree SDK importable without install.
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "xinsp2_py"))

from xinsp2 import Client  # noqa: E402


def main() -> int:
    url = os.environ.get("XINSP2_URL", "ws://127.0.0.1:8765")
    with Client(url) as c:
        c.open_project(HERE)
        rsp = c.call("open_project_warnings")
        warnings = rsp.get("warnings", [])

    print(f"open_project_warnings ({len(warnings)} entries):")
    for w in warnings:
        print(f"  [{w['instance']}] ({w['plugin']}) {w['reason']}")

    typo_warnings = [w for w in warnings if w["instance"] == "inst_typo"]
    clean_warnings = [w for w in warnings if w["instance"] == "inst_clean"]

    expected_kinds = {
        "unknown_config_key",
        "not_in_enum",
        "out_of_range",
        "type_mismatch",
    }
    seen = set()
    for w in typo_warnings:
        for k in expected_kinds:
            if w["reason"].startswith(k):
                seen.add(k)

    missing = expected_kinds - seen
    print()
    if clean_warnings:
        print(f"FAIL: clean instance produced {len(clean_warnings)} warning(s)")
        return 2
    if missing:
        print(f"FAIL: typo instance missed warning kind(s): {sorted(missing)}")
        return 1
    print("PASS: every expected warning kind was raised on inst_typo only.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
