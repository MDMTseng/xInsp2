# object_count_puzzle — a dogfood / known-answer vision regression

Locate and count dark blobs across 8 frames that degrade from clean to harsh
(blur + gaussian noise + salt-and-pepper + contrast compression + uneven
illumination). It's a fair-but-hard test: a naive *global* threshold fails on
the uneven-lighting frames, so the inspection script must use blur + local
threshold/background-flatten + morphology + shape filtering.

The point is **dogfooding xInsp2**: detection runs entirely inside the inspection
script (`inspect.cpp`, via the `xi::Image` cv ops) and is driven through the
backend over WebSocket — exactly how a real inspection works.

## Run (3 steps)
```
python generate_puzzle.py     # -> frames/*.png + ground_truth.json (deterministic seed)
python driver.py              # spawns the backend, compiles inspect.cpp, runs each
                              #   frame, writes predictions.json (count + centroids)
python score_puzzle.py        # greedy centroid match vs ground truth -> PASS/FAIL
```

A correct solution scores **count-exact on all 8 frames, precision/recall 1.0**.

## Files
- `generate_puzzle.py` — deterministic image + ground-truth generator (numpy/cv2).
- `inspect.cpp` — the xInsp2 inspection script: median+gaussian denoise →
  background-subtraction illumination flatten → Otsu → morphology →
  connected-components with area/shape filtering; emits `count` + `centroids`.
- `driver.py` — orchestrates the solve over WS (no detection logic here).
- `score_puzzle.py` — scores `predictions.json` against `ground_truth.json`.

`frames/`, `ground_truth.json`, `predictions.json`, and `detections_overlay.png`
are generated artifacts (gitignored) — regenerate with the steps above.
