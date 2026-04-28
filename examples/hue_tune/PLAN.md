# hue_tune — PLAN

Validate xInsp2's `exchange()` channel: a single plugin instance must change behaviour between `process()` calls without recompiling, just by sending JSON commands.

## Approach

- Generate 8 480x360 RGB frames with 6 red / 4 blue / 2 green saturated discs each, on a noisy gradient.
- One project plugin `hue_counter` reads `src` (3-channel), converts to HSV, masks pixels where `H ∈ [hue_lo, hue_hi]` AND `S >= 80` AND `V >= 80`, runs connected components, counts components with `area >= min_area`.
- Single instance `det`. Driver:
  1. `open_project` → `compile_and_load`
  2. exchange `reset` (red band 0..15, min_area=300), run 8 frames, expect 6.
  3. exchange `set_hue_range lo=110 hi=130`, run 8 frames, expect 4.
  4. exchange `set_hue_range lo=50 hi=70`, run 8 frames, expect 2.

## Open questions / sensible defaults

- **Color space from `xi::imread`**: stb_image yields RGB (host's `read_image_file` → `stbi_load`). Use `cv::COLOR_RGB2HSV` (NOT BGR2HSV).
- **OpenCV hue range**: 0-179. Red sits at 0 (and at 180 wrapping); pure blue at ~120; pure green at ~60. Picked test ranges accordingly.
- **Hue wrap**: not needed for the test (red band 0..15 doesn't wrap), so the plugin treats `lo..hi` as a simple inclusive range. Documented in plugin source.
- **Saturation/value floor**: hardcode 80 in the plugin (saturated discs are 255/255). Not a tunable for round 1.
- **Disc placement**: rejection-sample with min center distance >= 2*max_radius+4 to enforce no-overlap.
- **Driver parameter for `lo`/`hi` between sweeps**: use `c.exchange_instance("det", {...})`.

## Risk areas (where I expect friction)

- Whether `xi::imread` on the script side gives RGB or BGR (verified: RGB via stb_image).
- Whether `exchange_instance` exists in the SDK (verified: yes, `client.py` line 202).
- Whether mutating exchange state actually persists across `process()` without recompile (this is what we're testing).
- HSV thresholds + noise: σ=4 is small; saturated discs should still be H very close to 0/120/60, S=255, V=255.
