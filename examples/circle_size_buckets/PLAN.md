# PLAN — circle_size_buckets

## Task

Per frame, produce three int VARs: `count_small`, `count_medium`, `count_large`
matching ground truth. Targets: ≥ 12/15 frames perfect on all three; per-bucket
total error ≤ 5.

## Data

- 15 frames, 640×480 grayscale, gradient bg (50→200), σ≈10 noise.
- Each frame has 8–12 dark circles drawn from one of three bucket radii:
  - small  r ∈ [5, 8]    → area ≈ π·r² ∈ [78, 201]
  - medium r ∈ [11, 14]  → area ∈ [380, 615]
  - large  r ∈ [18, 22]  → area ∈ [1018, 1521]

The area gaps are wide (max-small=201 vs min-medium=380; max-medium=615 vs
min-large=1018). Classification by area is unambiguous if the binary mask
preserves circle area faithfully.

## Pipeline

Reuse the **`local_contrast_detector`** plugin from `circle_counting/` for the
binary mask (gradient-tolerant: dark-on-bright via local-mean subtraction). It's
already user-tunable, has a manifest, and a UI.

Write **one new project-local plugin** that does counting + size classification
in a single pass. Combining lets the plugin own the ledger of region areas
keyed by bucket and emit the three counts as outputs.

### Plugin: `size_bucket_counter`

Inputs: 1-channel binary `mask`.

Process:
1. Optional morphological close (radius slider) to seal noise gaps without
   merging neighbours. Default 1 (small — we MUST preserve r=5 circles).
2. `xi::ops::findFilledRegions` → list of pixel sets.
3. For each region: `area = points.size()`. Drop areas below `min_area`
   (noise) or above `max_area` (merged blobs / artefacts). Default
   `min_area=20`, `max_area=2200`.
4. Classify by area into buckets via two cutoffs:
   - small  : `min_area ≤ a < small_max`           default `small_max=290`
   - medium : `small_max ≤ a < medium_max`         default `medium_max=820`
   - large  : `medium_max ≤ a ≤ max_area`
   Cutoffs sit in the **gaps** between bucket area ranges so they tolerate
   morphology bleed and partial-circle slack.
5. Emit `count_small / count_medium / count_large`, plus `total_regions`,
   `rejected_small`, `rejected_big`, and a debug `cleaned` image.

Manifest in `plugin.json` covers all six params with min/max/default/doc.

### Inspect script (`inspect.cpp`) — orchestration only

```
input  = xi::imread(xi::current_frame_path())
mask   = det.process({src: input}).mask
out    = bucket_counter.process({mask: mask})
VAR(count_small),  VAR(count_medium),  VAR(count_large)
```

No image math at script scope — passes lambda smell rule.

## Tunables (user-visible)

`local_contrast_detector` (existing): blur_radius, block_radius, diff_C, polarity.
`size_bucket_counter` (new):           close_radius, min_area, small_max, medium_max, max_area.

UI: `ui/index.html` for `size_bucket_counter` shows sliders for all five and a
live count readout per bucket.

## Why one new plugin, not two (count + classify separately)

Splitting would require the counter to emit a list of region areas as JSON
and the classifier to re-parse it. Reusable in theory, but adds a JSON marshal
hop for no win on this task. If a future task needs raw areas, refactor then.

## Driver

`driver.py` opens the project, compiles inspect, runs all 15 frames via
`c.run(frame_path=...)` (no png_frame_source plugin needed, since we have
`xi::imread` + `current_frame_path`), tabulates per-bucket pred/truth/error,
and writes `RESULTS.md`.

Halfway through (after frame 7), it calls `c.image_pool_stats()` and
`c.recent_errors()` and includes the observations in RESULTS.md.

## Success criteria

- ≥ 12/15 frames perfect on `count_small` AND `count_medium` AND `count_large`.
- Sum of |pred-truth| across 15 frames per bucket ≤ 5.
