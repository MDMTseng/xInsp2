# qa_pack_param_modulation — parameters that change per frame ride IN the pack

The teaching example for one doctrine sentence:

> **Parameters that change at frame rate belong IN the pack (data plane), not
> in defs (control plane).**

## The three cadences — pick by rate

| Layer | Mechanism | Cadence | Use for |
|---|---|---|---|
| **def / commit_group** | `instance.json` config, `set_def`, commit_group | **Configuration** — quiesced, hundred-ms scale | The recipe: settings that hold across a run (a baseline threshold, a camera mode) |
| **pack entries** | an entry in the door-input pack, sealed per call | **Data** — every frame, zero coordination | Anything that varies frame to frame: a swept threshold, a per-frame ROI, gain from an upstream stage |
| **script params** | `exchange`, script-visible knobs | **Interactive** — human/UI scale | Operator tweaks while watching the line |

`commit_group` quiesces the pipeline to swap configuration atomically — that is
its job, and it is exactly why it is the *wrong* vehicle for a value that
changes every frame. A pack entry costs nothing extra: the pack is already
flowing, the parameter just rides along, and the door reads it with
`in.i64_or(key, def)` — **per-pack entry wins, def is the fallback**
(`toolbox/blob_analysis/blob_analysis.cpp`, xi.pack@1 door).

## What the example does

Per trigger the script builds a **stepped image** (four 4x4 squares at
intensities 60/110/160/210) and drives blob_analysis's pack door **twice**:

- **Leg A (data cadence)** — `threshold` rides in the pack, swept across
  frames through {40, 90, 140, 190, 240}. Because blob binarization is
  `pixel > threshold`, the blob count is a predictable **staircase** of the
  threshold: `4, 3, 2, 1, 0`. The assertion is that every frame's result
  reflects **that frame's own parameter**.
- **Leg B (configuration cadence)** — same frame, same door, **no** threshold
  entry: the door falls through to the def layer, set once by
  `instances/det/instance.json` `{ "threshold": 200 }` → always 1 blob,
  `threshold_used == 200`. (200 is deliberately *not* the plugin's compiled-in
  128 default, which would count 2 — so the run proves the def actually
  landed, not merely that a hardcoded fallback fired.)

Leg B runs **after** leg A in the same tick and is also the **no-leak proof**:
the sealed-pack-per-call model makes cross-frame leakage structural nonsense —
an entry lives only in the pack it was sealed into — but we assert it anyway.
If frame N's swept threshold ever contaminated the instance, leg B would echo
the swept value instead of the def; the driver additionally checks that every
consecutive wire-frame pair with *different* thresholds still matches each
frame's own staircase step.

## Run

```
python qa/qa_pack_param_modulation/driver.py
python tools/run_qa.py param_modulation
```

Requires the Windows backend + plugins built (`backend/build`,
`toolbox/build`, Release). Modeled on `qa/qa_use_pack_door/` (the pack
write-half flagship); the wire is expose's XEX1-v3, decoded by
`qa/lib/xex1.py`.
