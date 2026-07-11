# DOC_DEBT — the doc-debt ledger

**The contract:** any code change that invalidates a `docs/` page either fixes
the doc **in the same branch**, or adds one unchecked line here:

```
- [ ] <what changed> (<commit>) → <doc page/section to update>
```

The pre-merge gate (`tools/check_doc_debt.py`, wired as the `doc_debt` ctest
and into `gate.py --only docs`) **fails while any unchecked `- [ ]` item
remains** — debt may be recorded instead of paid immediately, but it cannot
ride a green gate to merge. When the doc is updated, tick the item (`- [x]`);
checked items are pruned periodically.

This is the manual companion to the derived guards: `check_doc_coverage.py`
catches an *undocumented* public symbol and `check_retired_terms.py` catches a
*retired* one still being taught, but neither can see a page whose prose is
simply now wrong. This ledger is where the human who made the change says so.

## Open debt

(none)
