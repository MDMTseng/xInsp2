# Circle counting v2 — results

Rebuilt the case as a real xInsp2 project: in-project plugins with UIs,
a `project.json`, instance defs, and an `inspect.cpp` that is pure
orchestration. Three project plugins replace the previous lambda +
hardcoded path code.

## Per-frame accuracy

| frame          | truth | pred | err |
|----------------|------:|-----:|----:|
| frame_00.png   |    10 |   10 |   0 |
| frame_01.png   |    10 |   10 |   0 |
| frame_02.png   |    10 |   10 |   0 |
| frame_03.png   |    10 |   10 |   0 |
| frame_04.png   |    10 |   10 |   0 |
| frame_05.png   |    10 |   10 |   0 |
| frame_06.png   |    10 |   10 |   0 |
| frame_07.png   |    10 |   10 |   0 |
| frame_08.png   |    10 |   10 |   0 |
| frame_09.png   |    10 |   10 |   0 |
| frame_10.png   |    10 |   10 |   0 |
| frame_11.png   |    10 |   10 |   0 |
| frame_12.png   |    10 |   10 |   0 |
| frame_13.png   |    10 |   10 |   0 |
| frame_14.png   |    10 |   10 |   0 |
| frame_15.png   |    10 |   10 |   0 |
| frame_16.png   |    10 |   10 |   0 |
| frame_17.png   |    10 |   10 |   0 |
| frame_18.png   |    10 |   10 |   0 |
| frame_19.png   |    10 |   10 |   0 |

**avg abs err: 0.000     exact: 20/20**  (target was ≤ 1.0)

## Plugin list and what each UI tunes

All three plugins live in-project under `examples/circle_counting/plugins/`.

### 1. `png_frame_source`
**Purpose:** load a PNG frame from a configurable directory by index.
The script calls `process({"idx": N})` and gets back an image keyed
`"frame"`. Vendor-bundled `stb_image.h` (single-header, public domain)
does the decode — the framework's existing `xi_image_pool.hpp` only
ships `stb_image_write.h`.

**UI tunes:**
- *Frames dir* (text field) — absolute path to the PNG folder.
- *Filename pattern* (text field) — uses `{idx}` as the placeholder.
- *Pad width* (number) — zero-pad width for `{idx}`.
- *Force grayscale* (checkbox) — pass `desired_channels=1` to stbi.
- *Probe* button — tries to load idx 0 and reports dims + path so the
  user can verify the directory + pattern without firing the script.

### 2. `local_contrast_detector`
**Purpose:** gradient-tolerant binary mask. Replaces the inline
`(bg - blurred) > C` lambda. Generalised so the same plugin handles
either polarity (dark spots on bright bg, or bright spots on dark bg).

**UI tunes:**
- *Blur radius* (slider 0..10) — gaussian smoothing radius, suppresses
  per-pixel noise before thresholding.
- *Block radius* (slider 3..120) — boxBlur window radius for the
  background estimate. Wants to be ≫ the largest target's radius so
  targets get smeared away when estimating "background".
- *diff_C* (slider 1..80) — minimum contrast vs local mean to mark a
  pixel as foreground.
- *Polarity* (radio) — dark-on-bright vs bright-on-dark.
- *Mask mean* readout — gives the user a numeric foreground fraction
  to tune by, since visual JPEG previews of binary masks look noisy
  (well-documented framework gotcha).

### 3. `region_counter`
**Purpose:** clean a binary mask and count regions whose area falls
within a window. Replaces the inline `close → findFilledRegions →
area filter` block.

**UI tunes:**
- *Close radius* (slider 0..12) — morphological close to bridge gaps
  inside each region.
- *Min area* / *Max area* (sliders) — area-based blob acceptance.
- Live readout of `count`, `total_regions`, `rejected_small`,
  `rejected_big` so the user can tell if the area filter is dropping
  real targets or letting noise through.

## Project layout

```
examples/circle_counting/
├── project.json                        ← cmd:open_project entry
├── inspect.cpp                         ← orchestration only (~40 lines)
├── ground_truth.json                   ← per-frame truth (existing)
├── frames/frame_00.png … frame_19.png  ← inputs (existing)
├── driver_v2.py                        ← Python harness for the eval
├── PLAN.md                             ← upfront plan
├── RESULTS_v2.md                       ← this file
├── instances/
│   ├── src/instance.json     plugin: png_frame_source
│   ├── det/instance.json     plugin: local_contrast_detector
│   └── counter/instance.json plugin: region_counter
└── plugins/
    ├── png_frame_source/
    │   ├── plugin.json
    │   ├── src/plugin.cpp
    │   ├── ui/index.html
    │   └── vendor/stb_image.h          ← vendored, see feedback #1
    ├── local_contrast_detector/
    │   ├── plugin.json
    │   ├── src/plugin.cpp
    │   └── ui/index.html
    └── region_counter/
        ├── plugin.json
        ├── src/plugin.cpp
        └── ui/index.html
```

## What `inspect.cpp` looks like now

```cpp
static xi::Param<int> frame_idx{"frame_idx", 0, {0, 9999}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    auto& src     = xi::use("src");
    auto& det     = xi::use("det");
    auto& counter = xi::use("counter");

    auto src_out = src.process(xi::Record().set("idx", (int)frame_idx));
    if (!src_out["loaded"].as_bool(false)) { VAR(count, -1); return; }
    auto frame = src_out.get_image("frame");
    VAR(input, frame);

    auto det_out = det.process(xi::Record().image("src", frame));
    VAR(mask, det_out.get_image("mask"));

    auto cnt_out = counter.process(xi::Record().image("mask", mask));
    VAR(count, cnt_out["count"].as_int(0));
}
```

No image math, no file I/O, no hardcoded paths. The user changes
behaviour by editing instance defs (or dragging sliders in the UI), not
by recompiling this file.

## Friction report — what was missing / confusing

These hit during this rebuild. The doc rules say "don't sugar-coat" so
in priority order:

### 1. PNG decoder still not in the public surface
Previous agent's #2 feedback is unchanged: `xi_image.hpp` ships
`Image(w,h,c,data)` but no `imread`, and the vendor dir has only
`stb_image_write.h`. I had to vendor `stb_image.h` into the plugin's
own `vendor/` folder. That's fine for a one-off, but a project-local
"load this PNG" plugin is the most common shape there is — bundling
`stb_image.h` next to `stb_image_write.h` and exposing a tiny
`xi::imread()` would remove this entire category of friction.

### 2. The `VAR(name, expr)` redeclaration trap is still there
Previous agent's #3 — and I tripped over it again on the very first
compile of `inspect.cpp`:
```cpp
auto mask = det_out.get_image("mask");
VAR(mask, mask);   // C2374: redefinition
```
The fix is `VAR(mask, det_out.get_image("mask"))`, but the docs in
`writing-a-script.md` don't call this out. Either:
- Change `VAR(...)` to be tolerant: `auto _xi_var_##name = (expr);` +
  push to the tracker, leave the user's local alone.
- Add a `VAR_TRACK(name, existing_var)` macro for the
  "I-already-have-it-named" case.
- At minimum, mention in `writing-a-script.md` that VAR declares.

### 3. `cmd:open_project` doesn't auto-compile the inspection script
Reasonable — they're separate concerns — but the on-ramp story
("user opens a project they got handed") implies it would. Current
flow forces every consumer (driver, UI, e2e test) to follow
`open_project` with `compile_and_load`. A flag or post-open hook would
remove the duplicated boilerplate.

### 4. No `cmd:open_project_warnings` reply structure documented
I called it on a hunch from `instance-model.md`. It either exists with
a different name or is missing — the call returned a `ProtocolError`
in my driver, which I had to wrap in try/except. `protocol.md` doesn't
list it. Either ship it or drop the references in the design doc.

### 5. `open_project` is slow + the SDK's default 30 s timeout is
**not** enough on a cold machine
With three project plugins to compile, the first `open_project` took
well over 30 s (each plugin gets a separate cl.exe invocation). The
SDK's default timeout fires and you get an opaque `Empty` from the
underlying queue. Either bump the SDK default, document the override,
or make `open_project` send a progress event so the client knows it's
still alive.

### 6. `instance.json` `config` parsing is stringly-typed
Backend's `detail_find_key` finds the `"config":` key and grabs the
matching `{...}` substring. It works, but it's a tiny ad-hoc parser
that assumes well-formed JSON without comments. A real `cJSON_Parse`
of `instance.json` would be more forgiving and let users add
`"_comment"` fields without breaking config load. Not blocking, just
brittle.

### 7. `xi::Param<T>` is scalar-only — same as before
This forced the frame source's `frames_dir` to be an instance-def
field, not a `Param`. That's actually the correct decision (paths
belong with the loader, not the script), but a `Param<string>`
would still help cases where the script wants a recipe name / a
tag / a label.

### 8. JPEG previews of binary masks are misleading
Previous agent's #6 — visible again here: when I dropped a `VAR(mask,
…)` to eyeball whether the mask polarity was right, JPEG ringing on
the 0/255 step makes it hard to tell. Numeric `mask_mean` (which I
expose from the detector plugin) is the workaround. A `subscribe`-style
"this VAR wants PNG previews" knob would be a one-line config win.

### 9. `cl.exe` build log encoding is mojibake on Chinese-locale Windows
The build log I had to read to debug the redecl error came back as:
```
error C2374: 'mask': ���Ʃw�q; �h�Ӫ�l�]�w
```
The compiler is emitting CP-950 / Big5; the build log file is being
written without an encoding annotation. The error code is enough to
work with, but the message is unreadable. Either set
`SET VSLANG=1033` before invoking cl.exe, or transcode the log when
saving it.

### 10. No "rebuild a project plugin without restarting the project" command was obvious
There IS `recompile_project_plugin` (I found it in `service_main.cpp`),
but it's not in `protocol.md` and the SDK has no wrapper for it. For a
"tune the pipeline live" workflow, hot-rebuild of project plugins is
the linchpin — it should be a top-billed protocol command.

## Iteration count

- Read framework docs / source: ~25 minutes
- Wrote three plugins + UIs: one shot each, no compile errors on the
  plugin side
- Wrote `inspect.cpp`: one compile error (the VAR redecl from feedback
  #2, fixed by inlining the expression)
- Wrote `driver_v2.py`: one timeout error (feedback #5, fixed by
  passing `timeout=180`)
- Final run: 20/20 first try once compiles passed

Total: 3 plugins + 1 script + 1 driver, with 2 framework friction
fixes inline. End-to-end accuracy matches the v1 result (20/20) but
the user can now retune blur / block / threshold / area filter
without recompiling anything by dragging UI sliders, and can repoint
the source at a different folder of frames by editing one instance
config field.
