# PLAN — blob_tracker

## Task

Drive 30 sequential frames through one xInsp2 inspection script, tracking
blob identities across frames so we can detect *left-to-right* crossings of
a vertical gate at `x = 320`. Final goal: after frame 29,
`total_crossings == ground_truth.total_crossings == 3`. Per-frame
`crossings_so_far` must also match (one-frame lag tolerated).

This is the first case where a stateless script can't work — frame N's
"is this a crossing?" depends on where the same blob was in frame N-1.

## Data shape

- 30 frames, 640×480 grayscale, gradient bg (60→200), Gaussian noise σ≈8.
- Up to 5 dark circular blobs per frame, radius 14, ~60 darker than local bg.
- Each blob has a fixed (start_x, y, speed). Speeds in {12, 18, 20, 4, 10};
  `y` ∈ {100, 200, 320, 60, 400} — distinct enough to use `y` as
  a near-unique track signature.
- Crossings to detect: 3 (blob 2 at frame 16, blob 1 at frame 17, blob 0 at
  frame 24). Blob 3 never reaches the gate; blob 4 starts past it.

The constant-y / per-blob-distinct-y structure makes nearest-neighbour
tracking robust: a frame-to-frame match by L2 distance finds the same
blob trivially as long as the detector returns it.

## Architecture

```
                                ┌───────────────────────────────┐
xi::imread(current_frame_path)──►   inspect.cpp                  │
                                │                                │
                                │  ┌── plugin "det" ───────────┐ │
                                │  │ blob_centroid_detector    │ │
                                │  │   blur→bg→(bg-src)>C       │ │
                                │  │   →close→regions→centroids │ │
                                │  └────────────────────────────┘ │
                                │                                │
                                │  tracker (in-script):           │
                                │   match cur ↔ prev by NN dist   │
                                │   if prev.x<gate && cur.x>=gate │
                                │       ++crossings               │
                                │   xi::state() ← {prev, crossings}│
                                └───────────────────────────────┘
```

### Why one plugin and not three

`circle_counting` had three plugins (source / detector / counter) because
counting is reusable. Here the "useful reusable op" is *centroid
extraction from a noisy gradient frame*. The bg-subtract + threshold +
morphology + region-centroid sequence belongs in one plugin so the user
gets one UI, one set of sliders, and one output (centroid list) that the
tracking script consumes directly.

The frame loader is **not** a plugin this time: the task specifically
asks us to use `xi::imread(xi::current_frame_path())` so the Python
driver controls the frame sequence purely via `c.run(frame_path=...)`.
That's also the natural pattern when a real camera / trigger source
becomes the input later.

### Why tracking lives in the script

Per the design rules: tracking is per-script semantics, not a reusable
image-math op. It's also the only piece that must talk to `xi::state()`,
and state lives in the script (the C++ ABI to push state from a plugin
is not the supported path).

## Plugin: `blob_centroid_detector`

Input: image `frame` (1-channel gray).

Process:
1. `gaussian(frame, blur_radius)` — denoise.
2. `boxBlur(blurred, block_radius)` → `bg` (local mean).
3. mask[i] = `(bg[i] - blurred[i]) > diff_C` → 0 or 255 (dark-on-bright).
4. `close(mask, close_radius)` → cleaned.
5. `findFilledRegions(cleaned)` → connected components.
6. For each region: area = `points.size()`. Drop if `area < min_area`
   or `area > max_area`. Compute centroid as mean(x), mean(y).
7. Emit a JSON array `centroids = [{x, y, area}, ...]`.

Outputs:
- image `mask`           — pre-morphology binary
- image `cleaned`        — post-close binary
- `centroids`            — JSON array (length = accepted regions)
- `total_regions`, `rejected_small`, `rejected_big`, `count` — diagnostics

Tunables (manifest-listed, sliders in UI):

| name           | default | range      | role                                |
|----------------|---------|------------|--------------------------------------|
| `blur_radius`  | 2       | 0..20      | gaussian denoise                    |
| `block_radius` | 40      | 1..200     | bg estimator window                 |
| `diff_C`       | 15      | 1..200     | dark-vs-bg threshold                |
| `close_radius` | 1       | 0..10      | morph close                         |
| `min_area`     | 200     | 0..2000    | reject below                        |
| `max_area`     | 4000    | 100..20000 | reject above                        |

`r=14` blob → area ≈ π·14² ≈ 615 px. So defaults `min_area=200`,
`max_area=4000` give a 3×–6× safety margin.

## Script: `inspect.cpp`

```
#define XI_STATE_SCHEMA_VERSION 1
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
```

Per call:

1. Read tunables (Param<int>): `gate_x` (default 320), `match_max_dist`
   (default 60 — generous, biggest per-frame jump in this set is 20 px).
2. Load frame: `imread(current_frame_path())`. If empty → emit error VAR.
3. `det.process({"src": frame})` → centroid list.
4. Read state JSON:
   - `prev_centroids` — array of `{x, y}` from last frame, or empty.
   - `crossings_so_far` — int, default 0.
   - `frame_seq` — int, increments each call, default 0.
5. Tracker: greedy nearest-neighbour. For each current centroid `c`:
   - Find unused prev `p` with min L2 distance to `c`.
   - If dist ≤ `match_max_dist`: that's the same blob. Test crossing:
     `p.x < gate_x && c.x >= gate_x` → ++crossings.
   - Else: new blob (just-appeared, e.g. came in from left edge).
6. Emit VARs: `frame_seq`, `n_blobs`, `crossings_so_far`,
   `delta_crossings`, `prev_blob_count`, plus the detector's images for
   visual inspection in the UI.
7. Write state back: replace `prev_centroids` with current, update
   counters.

### Why XI_STATE_SCHEMA_VERSION=1

Version `0` means "unversioned, restore blindly". I want a real bump-on-
change discipline from frame 0: if I later change `prev_centroids`'s
element shape (e.g. add `id` or `area`), bumping to `2` makes the
backend drop persisted state on the next compile_and_load and emit
`event:state_dropped`. That's safer than a partial default-fill that
silently miscounts crossings.

The `state_dropped` event is also useful as a *sanity check*: at the
start of a 30-frame run the driver compiles fresh; we expect either
no event (first ever load) or `state_dropped` (schema bump). Either
way, frame 0 sees empty `prev_centroids` and the math is right.

## Driver

`driver.py`:

1. Open project (`load_project` then `compile_and_load`).
2. Reset state: easiest way is to bump XI_STATE_SCHEMA_VERSION between
   dev runs OR just rely on the script's own logic (frame_seq starts at
   0 → first frame skips crossing check). Driver also explicitly calls
   a `cmd:set_state` if available; otherwise relies on schema bump.
   **Fallback that always works**: driver issues an `exchange_instance`
   reset, but state is owned by the script, not the instance. Cleanest:
   trust schema bump on dev iterations; for re-runs against the same
   compile, accept the per-run baseline (we count *frame-relative*
   crossings).
3. For each frame `00..29` in order: `c.run(frame_path=str(png))`.
4. Per frame: print `frame_idx | predicted_crossings | truth | status`.
5. After last: print final totals + image_pool_stats high-water.
6. Final pass criterion: `total_crossings == 3`.

## Risks / unknowns

- **State persistence on first ever compile** — whether the backend
  has any state to restore. Empty start is fine; the script's `state[k]`
  reads return defaults via `as_int(0)` etc.
- **Detector may briefly miss a blob** (drowned in noise) → tracker
  sees a "new" blob next frame. With NN matching at 60px tolerance
  per frame and the worst case being 20px/frame, a single miss is
  recoverable: when it reappears next frame the L2 dist is still
  ≤ 60 from the predicted prev. But: if the miss happens *exactly*
  when a blob is between frames N-1 and N straddling the gate, we
  could under-count. Mitigation: blur+threshold defaults are
  generous enough that all 5 blobs are detected on every frame in
  this synthetic data.
- **Greedy matching** — for 5 well-separated blobs, greedy NN is
  fine. Hungarian would be overkill.

## Pass bar

- `total_crossings == 3` exact.
- Per-frame predicted `crossings_so_far` matches truth, possibly
  with a single-frame lag if my detection-vs-update ordering is off.
- `image_pool_stats.cumulative.high_water` does not grow monotonically
  across the 30 runs (i.e. live_now drops between runs).
