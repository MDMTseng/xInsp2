#!/usr/bin/env python3
"""check_doc_links.py — the @doc code→doc backlink checker (bootstrap).

WHY THIS EXISTS
---------------
The doc guards all point ONE way: from a derived/declared surface toward the
docs. Nothing points the other way — when you are IN the code, there is no
machine-checked breadcrumb saying which doc page teaches this region, so the
person changing it has no prompt to update (or ledger, see DOC_DEBT.md) the
page they just invalidated. The convention this bootstraps: a source comment

    // @doc docs/reference/data-types.md#pack
    // @doc docs/guides/write-a-script.md

names the page (repo-relative path, optional #anchor) that teaches the code
around it. This check keeps those breadcrumbs HONEST: it scans
backend/include/xi + backend/src for @doc markers and fails if a referenced
file does not exist — a renamed/deleted doc page breaks the gate instead of
leaving a dangling pointer. Anchors are NOT validated yet (bootstrap: file
existence only; anchor checking can ratchet in once markers exist).

Zero markers is a PASS with a note — the convention is adopted organically,
region by region, not imposed in one sweep.

Run standalone (exits non-zero on a dangling link): python tools/check_doc_links.py
Optional repo-root override:                        python tools/check_doc_links.py <root>
Also wired as the `doc_links` ctest.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
SCAN_ROOTS = [ROOT / "backend" / "include" / "xi", ROOT / "backend" / "src"]
SCAN_PATTERNS = ("*.hpp", "*.h", "*.cpp", "*.c")

# `@doc <relpath>[#anchor]` on a COMMENT line (// or a /* ... */ body line) —
# requiring the comment marker keeps a string literal that merely contains
# "@doc" from being read as a backlink.
DOC_RX = re.compile(r"(?://|/\*|\*)\s*.*?@doc\s+([^\s#]+)(#[\w./-]+)?")


def main() -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    markers = 0
    scanned = 0
    dangling: list[tuple[str, int, str]] = []  # (rel, lineno, target)
    for base in SCAN_ROOTS:
        for pat in SCAN_PATTERNS:
            for f in sorted(base.rglob(pat)):
                scanned += 1
                rel = f.relative_to(ROOT).as_posix()
                for i, line in enumerate(
                        f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                    m = DOC_RX.search(line)
                    if not m:
                        continue
                    markers += 1
                    target = m.group(1).replace("\\", "/").strip()
                    if not (ROOT / target).exists():
                        dangling.append((rel, i, target))

    roots = ", ".join(r.relative_to(ROOT).as_posix() for r in SCAN_ROOTS)
    print(f"[doc-links] {markers} @doc marker(s) in {scanned} source file(s) "
          f"under {roots}")

    if dangling:
        print(f"\nDANGLING @doc link(s) ({len(dangling)}): each names a doc page "
              "that does not exist — fix the path or update the marker with the "
              "page's new home.\n")
        for rel, ln, target in dangling:
            print(f"  {rel}:{ln}: @doc {target}   -> no such file")
        print()
        return 1

    if markers == 0:
        print("[doc-links] OK — no @doc markers yet (the convention is adopted "
              "organically; this gate keeps them honest once they appear).")
    else:
        print("[doc-links] OK — every @doc backlink resolves.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
