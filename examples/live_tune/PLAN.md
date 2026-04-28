# live_tune — plan

## Read of the broken plugin

`plugins/circle_counter/src/plugin.cpp` does:

1. `gaussian(src, 2)` smooth.
2. `boxBlur(blurred, 40)` to estimate local background.
3. Threshold `bg - blurred > 18` → binary mask of the dark circles.
4. `erode(mask, 8)` — a radius-8 erosion. Circles in this dataset have
   radius 11-17 px. Erosion by 8 strips ~8 px from every edge, so:
   - r=11 circle becomes r=3 (area ≈ 28 px) — likely below the
     `area >= 50` floor → dropped.
   - r=17 circle becomes r=9 (area ≈ 254 px) — survives.
   Net: most frames drastically under-count.
5. `findFilledRegions` + area filter [50, 5000].

The local-contrast pass and the area filter are fine. The erosion is
the only knob to fix.

## Strategy

1. Open the project cold (`open_project`) — compiles the broken plugin once.
2. Compile this script (`inspect.cpp`).
3. Run all 10 frames with the broken plugin, log per-frame counts.
4. Edit `plugins/circle_counter/src/plugin.cpp`: drop erosion to
   `erode(mask, 1)` first. If under-count persists, drop erode entirely.
   If over-count appears (noise blobs surviving), add back radius 1-2.
5. `recompile_project_plugin("circle_counter")` after each edit; re-run
   the 10 frames; iterate until all frames hit 8.

## `recompile_project_plugin` shakedown

In addition to the convergence loop, deliberately introduce a syntax
error in the plugin source on one iteration to confirm:
- The cmd raises `ProtocolError` carrying / accompanied by a build log.
- The previous DLL stays loaded; the next `c.run()` still produces the
  old (broken) counts.
- After fixing and re-calling the cmd, the reply's `reattached` field
  lists `det`.

## Output

- `inspect.cpp` — load frame, call `det`, emit `count` + `mask`.
- `driver.py` — open project, compile script, run all frames, edit
  plugin, recompile, re-run, tabulate.
- `RESULTS.md` — score table per iteration, recompile shakedown notes,
  DX impressions vs prior cases.
