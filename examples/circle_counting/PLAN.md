# Circle counting v2 — plan

Rebuild the circle-counting case as a real xInsp2 project: in-project plugins
with UIs, a `project.json` the user can `cmd:open_project`, and an
`inspect.cpp` that is pure orchestration.

## Plugins (all in-project under `plugins/`)

1. **`png_frame_source`** — file-based image source.
   Why: replaces the hardcoded `FRAMES_RAW_DIR` + `frame_idx` Param hack.
   The script asks it for frame N; it reads `<frames_dir>/frame_NN.png`,
   decodes via vendored `stb_image.h`, returns it as an image in the
   output Record. Frames dir is an instance-def field (not a Param,
   because Param is scalar-only). Optional `pad_width` controls zero-pad
   width of the index in the filename.
   UI: pick frames directory (text field), set filename pattern, set
   pad width, click "Probe" to preview frame 0 + show its size & pixel
   stats. Lets the user point this at any folder of frames without
   recompiling.

2. **`local_contrast_detector`** — gradient-tolerant binary mask.
   Why: replaces the `(bg - blurred) > C` lambda inlined in the old
   `inspect.cpp`. Generalized: works for both dark-on-bright and
   bright-on-dark by a `polarity` flag. Internally does
   `gaussian(blur_radius)` → `boxBlur(block_radius)` → per-pixel
   `(bg - src) > C` (or the reverse) → binary.
   UI: sliders for blur_radius, block_radius, diff_C, polarity radio.
   Stats output (mean of resulting mask) lets the user see numerically
   whether their threshold is in the right ballpark.

3. **`region_counter`** — count blobs in a binary mask with area filter.
   Why: replaces the inline morphology + `findFilledRegions` + area-filter
   block. Plugin takes a binary mask and returns `{count, total_regions,
   rejected_small, rejected_big}` plus a colored visualization.
   Params: `close_radius`, `min_area`, `max_area`.
   UI: three sliders + a numeric readout of the last run's per-bucket
   counts so the user can tell whether they're rejecting too many at
   the small or large end.

## `inspect.cpp`

Pure orchestration: `xi::use("src").process({idx})` →
`xi::use("det").process({src})` → `xi::use("counter").process({mask})`
→ `VAR(count, ...)`. One `xi::Param<int> frame_idx` for the driver
loop. Nothing else. No image math, no file I/O, no hardcoded paths.

## `project.json`

Names the script `inspect.cpp`. Defines three instances (`src`, `det`,
`counter`) with sensible defaults that should give 20/20 out of the
box on the canned test set.

## `driver.py` (rewrite)

Open the project via `cmd:open_project`, `compile_and_load` the
script, loop over the 20 frames calling `set_param frame_idx N` +
`run`, score against `ground_truth.json`.
