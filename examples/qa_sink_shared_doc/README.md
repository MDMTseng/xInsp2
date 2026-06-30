# qa_sink_shared_doc — ordered-sink `$seq` shared-doc regression

Guards the host's **staged-sink flush** (`flush_staged_emits_` in
`backend/src/service_main.cpp`) against double-stamping the reserved `$seq` key into a
**shared** record doc.

## The bug

An ordered sink (`plugin.json` `"sink": true`) does not run inline under parallel
dispatch — the host *stages* each `xi::use(sink).process(rec)` call and flushes it after
the inspect, in frame order. On flush the host stamps `$seq` (the frame arrival id) onto
the delivered record so the sink can correlate the packet to its frame.

A script may stage the **same** `Record` to several sinks:

```cpp
xi::Record rec;
rec.set("measurement", frame);
xi::use("sinkA").process(rec);
xi::use("sinkB").process(rec);   // SAME rec → both staged items share ONE doc
```

`share_out` hands both staged items the *same* registry-refcounted `yyjson_mut_doc`. The
original flush stamped `$seq` **in place with `yyjson_mut_obj_add_int` (append, not put)
and no copy-on-write**, so:

- sinkA's flush appended one `$seq` to the shared doc,
- sinkB's flush appended a **second** `$seq` to that same doc → sinkB received a record
  with **two** `$seq` keys (malformed), and the doc a concurrent holder might still read
  was mutated underneath it.

## The fix

`flush_staged_emits_` now, per staged item:

- **COW only when shared** — if `DocRegistry::refcount(doc) > 1`, deliver a private
  `yyjson_mut_doc_mut_copy` and stamp that; the common single-sink path (rc==1, sole
  owner) still stamps in place with **no copy** (speed-first).
- **put-semantics** — `yyjson_mut_obj_remove_str(root, "$seq")` before the add, so a
  re-stamp or a doc that already carried `$seq` can never accumulate duplicate keys.

## How it probes

`plugins/count_sink` counts the `$seq` keys in each delivered record and reports the max
it ever saw via `exchange("stats")`. `driver.py` runs the two-sink script under the
synthetic timer and asserts **both** sinks report `max_seq_keys == 1`. Before the fix,
`sinkB` reported `2`.

```
python driver.py
# [sinkA] {'max_seq_keys': 1, 'calls': 160}
# [sinkB] {'max_seq_keys': 1, 'calls': 160}
# VERDICT: PASS
```
