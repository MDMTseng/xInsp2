# golden_defect — results

Validation run on `doc-cleanup` HEAD, in-proc backend (no isolation
flag). All 20 frames pass.

```
correct 20/20   TP=10 TN=10 FP=0 FN=0
mean IOU on TP frames: 0.913    mean centroid dist: 0.4 px
per-type recall: {'blob': 8/8, 'scratch': 2/2}
ACCEPTANCE: PASS
```

## What this case proves

- The two-phase workflow (setup-time reference load → run-time compare)
  is expressible as a real xInsp2 project.
- An AI agent can build that workflow autonomously from spec — three
  in-project plugins (`png_frame_source`, `reference_image`,
  `golden_defect_finder`), each with a working `ui/index.html`,
  glued by a 60-line `inspect.cpp`.
- `exchange_instance` works as the runtime-reconfigure channel: the
  reference plugin's UI button posts a `set_reference` exchange that
  swaps the reference without restarting the project.

## Per-frame numbers

See the run output (driver prints the full table). Key callouts:

- All 8 blob defects detected with **exact bbox** match in 6/8
  frames; the other 2 are 1-px off in one corner.
- Both scratch defects detected; bbox tight to within a few pixels.
- 10 clean frames all correctly classified as no-defect (no FP).

## Files

| File | Role |
|---|---|
| `project.json` | Root project file |
| `inspect.cpp` | ~60 lines orchestration (no image math) |
| `instances/{src,ref,finder}/instance.json` | Three instances |
| `plugins/png_frame_source/` | PNG loader (same shape as circle_counting) |
| `plugins/reference_image/` | Holds the reference; `set_reference` exchange swaps at runtime |
| `plugins/golden_defect_finder/` | Diff-and-threshold logic, returns mask + bbox + score |
| `driver.py` | Drives all 20 frames, scores against `ground_truth.json` |
| `frames/` + `reference.png` + `ground_truth.json` | Test set |

## Pipeline (per frame)

```
src.process({idx})            -> frame image
ref.exchange("get")           -> reference image (cached after first load)
finder.process({frame, ref})  -> mask + bbox + score
```

`finder` does: gaussian blur both → abs-diff → adaptive threshold →
morphological cleanup → connected components → keep largest, return
its bbox + area. Single tunable threshold (`diff_floor`) does the
heavy lifting; `min_area` filters speckle noise.

## Backend stability note

This case was originally killed mid-run when the backend AVed inside
the `reference_image` plugin path on an in-proc setup (the issue
`docs/testing.md` calls out as a known limitation: plugin AV in the
in-proc layout can take the backend with it). The fix the AI agent
arrived at — caching the reference image in plain `xi::Image` rather
than re-decoding per call — sidesteps the original crash; the run is
now stable.

If you want hard isolation (worker-per-plugin), see PR #1 / the
`process-isolation` branch. That work is opt-in for now; circle and
golden cases run fine with default in-proc.
