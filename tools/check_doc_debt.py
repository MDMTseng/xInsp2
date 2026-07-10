#!/usr/bin/env python3
"""check_doc_debt.py — the doc-debt ledger gate (docs/DOC_DEBT.md).

WHY THIS EXISTS
---------------
The derived doc guards cover what they can DERIVE: check_doc_coverage asserts
every live public symbol is taught, check_retired_terms asserts no retired
token is taught. Neither can see a page whose PROSE is now simply wrong — a
renamed concept, a changed default, a flow that gained a step. Historically
that class of drift shipped silently and was caught rounds later by manual
review (the same failure mode both derived guards were built against).

docs/DOC_DEBT.md is the manual channel: a code change that invalidates a doc
page either fixes the doc in the same branch, or records one line of debt:

    - [ ] <what changed> (<commit>) → <doc page/section to update>

This check makes that ledger BINDING: it fails while any unchecked `- [ ]`
item remains, so debt can be recorded instead of paid immediately, but it
cannot ride a green gate to merge. Checked items (`- [x]`) pass and are pruned
periodically. It also rejects malformed checkbox lines (`- []`, `- [X ]`, ...)
so a typo cannot smuggle an open item past the scan.

A missing docs/DOC_DEBT.md is a PASS with a note, not a failure — the ledger
is contract, not ceremony; an empty/absent ledger means no known debt.

Run standalone (exits non-zero on open debt): python tools/check_doc_debt.py
Optional repo-root override:                  python tools/check_doc_debt.py <root>
Also wired as the `doc_debt` ctest.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
DEBT_FILE = ROOT / "docs" / "DOC_DEBT.md"

# A line that BEGINS a checkbox must be exactly one of these two shapes.
# Anything else starting `- [` (e.g. `- []`, `- [x ]`, `- [X]`) is a typo that
# would otherwise silently fall out of the scan — reject it loudly instead.
OPEN_RX = re.compile(r"^- \[ \] ?(.*)$")
DONE_RX = re.compile(r"^- \[x\] ?(.*)$")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    if not DEBT_FILE.exists():
        print(f"[doc-debt] OK — no {DEBT_FILE.relative_to(ROOT).as_posix()} "
              "(no ledger means no recorded debt).")
        return 0

    open_items: list[tuple[int, str]] = []
    malformed: list[tuple[int, str]] = []
    done = 0
    in_fence = False  # the ledger's own header SHOWS the line format in a
    #                   ``` fence; a format example is not debt.
    for i, line in enumerate(
            DEBT_FILE.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        s = line.strip()
        if s.startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence or not s.startswith("- ["):
            continue
        if OPEN_RX.match(s):
            open_items.append((i, s))
        elif DONE_RX.match(s):
            done += 1
        else:
            malformed.append((i, s))

    rel = DEBT_FILE.relative_to(ROOT).as_posix()
    print(f"[doc-debt] {rel}: {len(open_items)} open, {done} done, "
          f"{len(malformed)} malformed")

    if malformed:
        print("\nMALFORMED checkbox line(s) (must be exactly `- [ ]` or `- [x]` — "
              "a typo here would silently escape the gate):")
        for ln, text in malformed:
            print(f"  {rel}:{ln}: {text}")

    if open_items:
        print(f"\nOPEN doc debt ({len(open_items)}): fix each doc and tick the item "
              "`- [x]` (prune checked items periodically).\n")
        for ln, text in open_items:
            print(f"  {rel}:{ln}: {text}")
        print()

    if open_items or malformed:
        return 1
    print("[doc-debt] OK — no open doc debt.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
