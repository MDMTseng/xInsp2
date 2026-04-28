# trend_monitor — PLAN

## Task

Process 30 frames in order. Each frame has ~5 dark circles on a noisy
gradient bg. Per-frame report `{count, running_mean, flagged: bool}`.
A frame is flagged when its circle count deviates more than 30% from
the running mean of the previous 10 frames. Ground truth marks
frames 12, 18, 24 as anomalies (counts 12 / 1 / 11).

## Decomposition

Two layers:

1. **Per-frame circle count** — pure image-math; reusable. Lives in a
   plugin (`blob_centroid_detector`, copied from `examples/blob_tracker`).
   The blob detector is a one-pass `gaussian → local-mean subtract →
   threshold → close → label → area filter → centroid`. Same data
   profile as blob_tracker (radius 10-18, sigma=8 noise, dark blobs on
   gradient bg) so the defaults transfer.

2. **Trend / flag logic** — per-script semantics, not reusable. Lives
   in `inspect.cpp`. Maintains `xi::state()["window"]` (rolling array
   of recent counts, capped at `window_size=10`).

## Why a plugin for the count

The task explicitly requires "a real plugin with `manifest` for the
circle-count logic (no inline image-math in inspect.cpp)". Image-math
is also exactly the kind of thing plugins are designed for: it has
tunable parameters (blur, threshold, area limits) the user might want
to live-tune via the params strip, and the same plugin already exists
proven in the blob_tracker example.

## State shape (schema 2)

```
xi::state() = {
  "window":    [int, ...]    // most-recent counts, oldest first, ≤ window_size
  "frame_seq": int            // monotonic counter
}
```

`XI_STATE_SCHEMA(2)` at file scope. Bumped to 3 mid-development to
verify the `state_dropped` event path.

## Flagging logic

```
if window.size() < warmup_n:     flagged = false   (warm-up)
elif mean == 0:                  flagged = false   (degenerate)
else:
    deviation = |count - mean| / mean
    flagged   = deviation > deviation_pct/100
```

If a frame is flagged, **its count is not appended to the window**.
Rationale: a one-off anomaly with count=1 or count=12 would shift the
window mean enough to mask a later anomaly. The dataset has 3 isolated
anomalies (12, 18, 24) so skip-on-flag is safe. Caveat noted in code:
this policy fails on a real level shift — we'd want a "drift confirmed"
path for production.

Warm-up policy: **NO-FLAG** for the first 10 frames (window size
< warmup_n=10). Task allows either no-flag or early-rejection;
no-flag makes the trace easier to read.

## Tunables (live-tune surface)

Script params:
- `window_size` (default 10) — task spec
- `warmup_n`    (default 10) — same; can decouple if wanted
- `deviation_pct` (default 30) — task spec

Plugin params (`det` / blob_centroid_detector): blur_radius, block_radius,
diff_C, close_radius, min_area, max_area — all unchanged from
blob_tracker defaults, which were tuned for the same data profile.

## Driver flow

1. `open_project(folder)` — first call.
2. `compile_and_load(inspect.cpp)` — schema 2.
3. Run frames 0..14.
4. `open_project(folder)` again — verify no SIGSEGV.
5. Recompile, probe state; if reset, re-prime window by replaying 0..14.
6. Run frames 15..29 — score against ground truth.
7. Bump `XI_STATE_SCHEMA(2)` → `XI_STATE_SCHEMA(3)` in inspect.cpp.
8. Recompile.
9. **Before** next run, drain events from `_inbox_events` and assert
   one is `state_dropped` with `{old_schema:2, new_schema:3}`.
10. Run a probe frame; assert `window_len_in == 0` (state was dropped).
11. Restore schema to 2 for the next session.
