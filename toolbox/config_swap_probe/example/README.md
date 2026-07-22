# config_swap_probe — example project

Swapping a plugin's config **while product is moving**, without a stall and
without a torn frame.

A plugin holds a heavy resource — model weights, a calibration table, an SBM
template. Load it onto the live pointer and every frame in flight queues up
behind the load; do it carelessly and one frame reads half the old table and
half the new one, and you get a defect you will never reproduce.

`config_swap_probe` is the reference for the answer: a **double slot**.

```
active_   the LIVE slot process() reads          (atomic pointer)
staged_   the BACKGROUND slot prepare() builds   (untouched by live traffic)
commit()  one atomic swap: active_ = staged_
```

Run it and the frame stream partitions into exactly three states:

```
phase A (42,none)=7   B (42,staged)=4   C (99,none)=6
```

**What it shows**

- `prepare_instance` builds the new resource in the staging slot **concurrently
  with live traffic**. Phase B is the whole design: the new config is fully
  loaded and sitting there, and frames still see the old one. Nothing paused.
- `commit_group` drains dispatch, calls `commit()` in a no-process window, and
  resumes. No frame is in flight across the swap, so no frame can see half of
  it.
- the per-frame atomicity check is one comparison: `last_seen == active`.
  `last_seen` is the config value the door observed *during* that frame;
  `active` is the live slot read back a moment later. If a swap could land
  mid-frame those would disagree. They never do — and the driver checks it on
  every frame, not on the last one.
- the script drives the plugin through its `xi.pack@1` door with the trigger's
  own sealed pack — no adapter, no conversion — and reads its control plane
  through `exchange`. A door with no output answers with an empty-but-valid
  pack; empty is not failure.
- `set_instance_def` is the other, simpler path: load and swap in one step, host
  serialized. Correct, but it stalls for the length of the load. prepare/commit
  exists to buy that time back.

```
python tools/run_qa.py example_config_swap_probe
```

The driver asserts the negative half explicitly: phase B must be **non-empty**,
and every frame in it must still observe `42`. A staged config that leaked into
live traffic would be caught there. A test that only checked "after commit the
frames see 99" would pass equally well against a plugin that swapped on
`prepare` and stalled the pipeline doing it — which is exactly the design this
plugin exists to replace.

**Files**: `project.json` (cam + probe + expose), `instances/`, `inspect.cpp`,
`driver.py`. The `swap` channel in the webUI shows the phase live.

See also `qa/qa_pack_config_swap/` — the same swap as a regression test, with
frame-loss accounting across the commit barrier.
