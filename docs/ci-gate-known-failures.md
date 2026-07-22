# CI gate — the qa quarantine

## Why this exists

`tools/gate.py` is the day-1 pre-merge gate. When it was first stood up it did
exactly what a gate is for: it ran the whole enforced surface and found that the
`qa/qa_*` regression suite was **already red on master** — 13 of 25 tests
failing — before any of this branch's changes. The CI-gate branch does not own
those examples, so the failures are quarantined rather than fixed here.

Standing up a gate on a partially-broken suite has two honest options: block
everything until the suite is perfect, or run the gate now and **quarantine the
known-broken tests loudly** so it still catches *new* breakage. This project
takes the second path. A quarantine is a tracked, self-emptying skip-list — not
a mute.

## How it works (mechanism, not a list)

The live list is **[`qa/qa_known_failing.txt`](../qa/qa_known_failing.txt)**
(`<qa_dir>: <reason>` per line). Do not duplicate it here — that copy would rot.

`tools/run_qa.py` reads it and:

- still **runs** every quarantined test;
- a quarantined **failure** prints a loud `KNOWN-FAIL (<reason>)` and does **not**
  fail the suite;
- a quarantined **pass** prints `UNEXPECTED PASS` and **does** fail the suite —
  so when the owning branch fixes a test, the gate forces its line to be deleted
  (the list cannot silently rot green);
- a **non-quarantined** failure is fatal, exactly as before.

`python tools/run_qa.py --strict` (and the same via the gate) ignores the file
entirely and treats every failure as fatal — the raw truth, and the eventual
clean-suite gate once the quarantine is empty.

## What's in it today, and who empties it

Twelve of the thirteen are **dead-API scripts**: their `inspect.cpp` still uses
the retired `VAR(...)` / `EMIT(...)` macros and the legacy
`xi_inspect_entry(int frame)` entry, all removed from the SDK headers, so they no
longer compile (`C3861 'EMIT'/'VAR' not found`, `C2065 'frame' undeclared`).
Rewriting or deleting those scripts is **adoption-map item 4** ("fix or delete
the 32 dead-API scripts") — a separate branch. As item 4 lands, each fixed test
starts passing, trips the `UNEXPECTED PASS` guard, and its line comes out. The
one non-dead-API entry (`qa_lifecycle_teardown`) is a separate pre-existing
failure on master with no dead-API tokens; its owner is still to be assigned.

**End state:** the quarantine shrinks to empty, and the gate's qa stage flips to
`--strict`.
