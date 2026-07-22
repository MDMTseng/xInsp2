# qa_pack_order — arrival-ordered PACK pushes under a parallel pool (U3)

The pack-only successor to `qa_result_order` (docs/new_gen/17; flips parity-
matrix row C3). Same uneven-timing workload (every 5th frame slow,
`dispatch_threads=4`), but each frame's output is a script-built sealed pack
(`xi::ScriptPackBuilder`) pushed to the `expose` sink via
`xi::use("expose").push(pack)` — no `xi::Record` anywhere.

Ordering per the doc-17 contract:

- **Delivery order — envelope.** `push()` on a declared ordered sink is staged
  and flushed inside the per-lane emit gate, so wire order follows
  `parallelism.result_order`: `"arrival"` replays frame order, `"completion"`
  emits as workers finish.
- **Identity — producer-stamped.** Sealed packs are immutable; the host never
  stamps them. The script stamps the host arrival id itself before seal:
  `b.add_i64("$seq", (int64_t)xi::run_id())` — the same value the Record-era
  host stamp injected at flush.

Doctrine probe (doc 17 §b): each frame also calls `use("expose").process(pack)`
— rejected on a declared sink (rc −5 → empty pack; the sink feed is `push()`).
The rejection is recorded in the pushed pack (`probe_rejected`) and asserted on
every frame.

`driver.py` runs both modes and reads each XEX1 frame's `seq` (= the pushed
`$seq` = `run_id`) in receive order:

- **completion** → reorders (inversions > 0), proving the workload really does
  finish out of order under this pool;
- **arrival** → zero inversions on that same workload;
- every frame: `seq > 0` (run_id flowed) and `probe_rejected == 1`.

```
python driver.py     # VERDICT: PASS
```

Windows-only (plugin/script compile); skips on non-`nt`. See
`docs/new_gen/17-pack-ordering-semantics.md` and
`docs/guides/write-a-script.md` (Parallel dispatch → `result_order`).
