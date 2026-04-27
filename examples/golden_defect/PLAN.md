# golden_defect — plan

## Problem shape

Two-phase workflow, unlike circle_counting:
- **Setup phase** (once): user loads a reference "golden" image.
- **Run phase** (per frame): test frame is compared against the reference;
  output `defect_present` + bbox + confidence.

The reference is *not* per-frame data, so it can't ride on the input
record like the source frame does. It needs to live in an instance that
holds it across runs.

## Plugins

### 1. `png_frame_source` (per-frame loader)

Same pattern as circle_counting. `process({idx})` -> loads
`<frames_dir>/frame_{idx:02d}.png`, returns the grayscale image. Path
in instance.json. Reusable as-is from circle_counting (vendor stb_image).

UI: pick frames dir, pattern, pad width, force grayscale toggle, probe
button. (Same as circle_counting.)

### 2. `reference_image` (the setup-time plugin)

The interesting one. Holds the golden reference image in memory after a
one-time load.

- `instance.json` config: `{ "reference_path": "<abs path>" }`.
- On `set_def` (load_project / instance create), if path non-empty,
  attempt to load the PNG into a cached `xi::Image`. Record success/error.
- `process(input)`: returns `Record().image("reference", cached_)
  .set("loaded", bool).set("path", ...).set("width", w).set("height", h)`.
  No per-frame cost — just hands back the cached `xi::Image` (shared_ptr
  pixels, cheap).
- `exchange`:
  - `get_status`: returns get_def() (also includes loaded/dims).
  - `set_path` { value: "<path>" }: update path AND immediately reload.
  - `reload`: re-read from current path (useful if file changed).

UI (`ui/index.html`): the *user's setup screen*. Shows:
- big "loaded / not loaded" status indicator (green / red).
- current path + dimensions.
- text input + "Load Reference" button -> sends `set_path` with the typed
  path. Updates state immediately.
- "Reload" button.

Note: the user can also point `instance.json` at a path before opening
the project — both work, since `set_def` is called on project open.

### 3. `golden_defect_finder` (the run-time matcher)

Takes two images on the input record (`reference` and `frame`),
returns defect detection.

Algorithm (chosen for the test set's nature: smooth gradient + texture
+ small dark blob OR thin dark scratch on top of gaussian noise):

```
diff   = |frame - reference|       // absolute difference (uint8)
blur   = gaussian(diff, blur_radius)   // suppress per-pixel noise sigma~8
mask   = blur > diff_threshold     // binary
mask   = close(mask, close_radius) // join near pixels (helps thin scratches)
regions = findFilledRegions(mask)
keep regions with min_area <= area <= max_area
defect_present = any kept region
bbox = bounding box of largest kept region
score = max region area / min_area  (rough confidence)
```

Why blur the diff (not the inputs): with σ≈8 noise, per-pixel diffs are
mostly noise; averaging over a small window gives a much cleaner signal.
A 50-level dark blob and a 35-level dark scratch both survive a 2-3 pixel
blur and exceed any sensible threshold; pure gaussian noise post-blur
should sit well below.

Thin scratches (~4 pixels wide) need `close_radius` >= 2 so they form a
contiguous region; otherwise area filter discards them.

UI: sliders for blur_radius, diff_threshold, close_radius, min_area,
max_area. Numeric readouts: total_regions, kept, largest_area, last bbox.

## inspect.cpp (orchestration only)

```cpp
xi::Param<int> frame_idx{"frame_idx", 0, {0, 19}};

void xi_inspect_entry(int) {
    auto& ref    = xi::use("ref");
    auto& src    = xi::use("src");
    auto& finder = xi::use("finder");

    auto ref_out = ref.process(xi::Record{});
    bool ref_loaded = ref_out["loaded"].as_bool(false);
    VAR(reference_loaded, ref_loaded);
    if (!ref_loaded) {
        VAR(error, std::string("reference not loaded"));
        VAR(defect_present, false);
        return;
    }

    auto src_out = src.process(xi::Record().set("idx", int(frame_idx)));
    if (!src_out["loaded"].as_bool(false)) {
        VAR(error, src_out["error"].as_string("frame load failed"));
        VAR(defect_present, false);
        return;
    }
    auto frame = src_out.get_image("frame");
    auto reference = ref_out.get_image("reference");
    VAR(input, frame);
    VAR(reference, reference);

    auto out = finder.process(xi::Record()
        .image("reference", reference)
        .image("frame", frame));

    VAR(diff,           out.get_image("diff"));
    VAR(mask,           out.get_image("mask"));
    VAR(defect_present, out["defect_present"].as_bool(false));
    VAR(score,          out["score"].as_double(0.0));
    VAR(largest_area,   out["largest_area"].as_int(0));
    VAR(bbox_x0,        out["bbox_x0"].as_int(-1));
    VAR(bbox_y0,        out["bbox_y0"].as_int(-1));
    VAR(bbox_x1,        out["bbox_x1"].as_int(-1));
    VAR(bbox_y1,        out["bbox_y1"].as_int(-1));
}
```

## Files

```
examples/golden_defect/
├── PLAN.md                       (this file)
├── RESULTS.md                    (after running)
├── project.json
├── inspect.cpp
├── reference.png                 (already there)
├── frames/                       (already there)
├── ground_truth.json             (already there)
├── instances/
│   ├── ref/instance.json         (reference_path = ../../reference.png)
│   ├── src/instance.json         (frames_dir, pattern)
│   └── finder/instance.json      (default tuning)
├── plugins/
│   ├── png_frame_source/         (copied from circle_counting)
│   ├── reference_image/
│   └── golden_defect_finder/
└── driver.py                     (loops 20 frames, scores)
```

## Acceptance target

- 20-frame walk: count TP+TN >= 18, FP <= 1, FN <= 1.
- Default tuning should hit that target without UI intervention.
- Bbox: don't formally score for acceptance, but report mean IOU /
  centroid distance vs ground truth in RESULTS.md.

## Friction notes (collect during build)

Track: anywhere docs/SDK/ABI made me guess; `exchange_instance` /
`set_instance_def` corner cases; `recompile_project_plugin` use; whether
the reference-loading workflow felt "real" or hacky.
