# Circle counting — results

## Final accuracy

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

**Average absolute error: 0.000  Exact-match frames: 20/20.**

## Final pipeline

`gray -> gaussian(blur_radius=2) -> bg = boxBlur(blurred, block_radius=40) ->
binary = (bg - blurred) > diff_C=15 -> close(close_radius=2) -> findFilledRegions
-> filter by area in [min_area=300, max_area=4000] -> count`.

Why each piece:

- **gaussian(2)** smooths the σ≈12 gaussian noise enough that the per-pixel
  comparison below is dominated by the real circle signal (~60 levels)
  rather than noise.
- **wide boxBlur(40)** estimates local background. With a 81×81 window vs
  circles of radius ≤26, individual circles are smeared away — the
  result tracks the gradient.
- **(bg - blurred) > 15** marks pixels where the local pixel is at
  least 15 grayscale levels darker than the surrounding mean. This is
  exactly the gradient-tolerant defect mask the task needs.
- **close(2)** fills small noise-induced pinholes inside each circle so
  the region is a single connected blob.
- **area filter [300, 4000]** keeps anything between ≈ π·10² and π·36²,
  which comfortably covers the spec'd radius range 14..26 plus
  morphology's slight inflation, while throwing away noise-tail
  components.

## Final parameter values

| param          | value | notes |
|----------------|-------|-------|
| `blur_radius`  | 2     | gaussian(2) is enough vs σ=12 noise |
| `block_radius` | 40    | window = 81 px > circle diameter (52 px) |
| `diff_C`       | 15    | < signal (60) but > noise (~12·√window) |
| `close_radius` | 2     | enough to bridge noise speckles |
| `min_area`     | 300   | π·14² ≈ 615; allow some erosion |
| `max_area`     | 4000  | π·26² ≈ 2124; allow some dilation |

## Process / sequence of approaches

1. **Reading the framework.** Skimmed `SKILL.md`, `docs/README.md`,
   `docs/guides/writing-a-script.md`, `examples/defect_detection.cpp`,
   `backend/include/xi/xi_ops.hpp`. The defect example pulls from a
   `TestImageSource`; this task gives a path. Inspected
   `service_main.cpp` and confirmed: **the backend's `cmd:run` accepts
   `frame_path` over the wire but does NOT forward it to the script.**
   Only the frame *index* is passed (`run_one_inspection(... frame_hint)`),
   and `xi::Param`s only support scalars (no `Param<string>`).

2. **Decided to load the frame myself in C++.** The vendor dir only
   ships `stb_image_write.h`, no `stb_image.h`, so I can't decode PNG
   in-script. Instead I sidestepped: a Python driver (`driver.py`)
   converts each PNG to a flat 640×480 `frame_NN.raw` once, the script
   reads `frame_<frame_idx:02>.raw` from a fixed dir, and `frame_idx`
   is set via `set_param` per run.

3. **Algorithm v1: invert + adaptiveThreshold + close + count regions
   with area filter.** Compile failure: I'd written
   `xi::Image x = …; VAR(x, x);`, but `VAR(name, expr)` *declares*
   `auto name = …`, so the second `VAR(x, x)` was a redeclaration.
   Fixed by writing the body as `VAR(blurred, gaussian(...))` etc.

4. **Algorithm v1, second run (block_radius=30, diff_C=15).** Counted
   3 regions, all rejected. Dumping the binary image showed circles
   appearing as DARK rings on a white background — i.e., almost the
   whole image was being labelled foreground. Bumped block_radius=60,
   close_radius=4 — now total_regions = 1 (single huge blob, mean=255).

5. **Built a Python prototype (`proto.py`) using
   scipy's `uniform_filter` and `binary_*`.** The shape
   `(local_mean - pixel) > C` worked perfectly. Sweeping
   block_r ∈ {40,60,80,100}, C ∈ {5,10,15,20,25}, close_r ∈ {2..6}
   produced multiple configurations with 20/20 exact match. Top:
   `block_r=40, C=10, close_r=2`.

6. **Algorithm v2: do the same in C++ explicitly.** I was suspicious
   of `xi::ops::adaptiveThreshold`'s output (it didn't match the
   Python `(mean-src) > C` semantics in my run), so I just wrote the
   masking inline as a lambda over `boxBlur(blurred, block_radius)`
   and `blurred`. First try — frame 0 → 10 regions, count=10. Full
   sweep — **20/20 exact**.

## Things that didn't work / dead-ends

- **Using `xi::ops::adaptiveThreshold(invert(blurred), block_r, C)`**
  in place of the manual mask: in my hands the cleaned-binary mean came
  back as 254.7 (almost all white) even with block_radius=40 and the
  same C that worked perfectly in pure-numpy. I didn't dig into why —
  just sidestepped with the explicit inline mask. Possible suspect:
  `boxBlur`'s integer-sum / division behaviour at low pixel values, or
  some interaction with `invert` and the JPEG preview encoding I was
  reading visually. Left as feedback below.

- **Trying to use the wire-level `frame_path` directly.** The
  protocol fixture and `Client.run(frame_path=…)` both accept it, but
  the backend's `run` handler in `service_main.cpp:1008-1031` ignores
  the args and just calls `run_one_inspection(srv, frame_hint=1, …)`.
  The script has no `xi::current_frame_path()` analogue. The clean
  solution would be either to wire `frame_path` through to the script
  (e.g. via a thread-local string accessor) or to provide a
  one-liner `xi::imread(path)`.

## Iteration count

Five compile-and-load runs total:

1. Initial skeleton — failed: `VAR` redeclaration errors.
2. Fixed VAR usage — compiled, but counted 0 (block_radius=30, C=15,
   close_radius=1).
3. Increased block_radius=60, close_radius=4 — counted 0
   (single huge blob).
4. Switched to inline mask + matched the Python-tuned params — first
   frame counted 10.
5. Cleanup pass (removed sanity VAR) — 20/20 final.

Plus four pure-Python prototype iterations on `proto.py` for parameter
discovery (no backend calls).

## Feedback on SDK / docs / framework

These were the rough edges in this task. None blocked progress.

1. **No way to get `frame_path` into the script.** The `cmd:run`
   protocol carries it, the Python client passes it, but the backend's
   `run` handler discards it. Documented or not, this is misleading —
   I assumed for a while that `xi::current_frame_path()` or similar
   existed and just couldn't find it. A `xi::current_request()` /
   `xi::frame_path()` accessor would be a natural fix, *or* the docs
   should explicitly say "scripts pull from a source, frame_path is
   ignored — use a TestImageSource."

2. **No PNG decoder in the public surface.** `xi_image.hpp` has
   `Image(w,h,c,data)` but no `imread`. The vendor dir ships
   `stb_image_write.h` only. For a "load this PNG and inspect it"
   workflow this is the very first thing you reach for. Either add
   `xi::imread()` (stb_image is ~2k lines, public domain) or
   document the convention "convert your test fixtures to raw and
   set a frame_idx param" so people don't waste time looking for
   what isn't there.

3. **`VAR(name, expr)` collides with prior local variables.** The
   macro expands to `auto name = …`. So the natural pattern
   ```cpp
   xi::Image binary = adaptiveThreshold(...);
   VAR(binary, binary);          // C2374 redeclaration
   ```
   doesn't work — you have to either (a) inline the expression
   directly into `VAR()`, or (b) rename the local. The
   `writing-a-script.md` example uses pattern (a) (`VAR(blur,
   gaussian(gray, sigma))`) but never calls out that (b) is needed
   if you've already named the value. A one-line note in the doc, or
   adding a `VAR_TRACK(name, existing_var)` macro that doesn't
   re-declare, would have saved a compile cycle.

4. **`xi::Param` is scalar-only.** Specifically no `Param<string>`,
   so I couldn't pass the frame path. Combined with #1, this forced
   the `frame_idx` + hardcoded directory pattern. Adding string params
   would let inspections be parameterised on file paths /
   recipe names without resorting to side files.

5. **`ProtocolError` from `compile_and_load` says only "compile failed"
   in the message** (the build log is supposedly in the message but
   wasn't visible from the repr). I had to go dig under
   `%TEMP%\xinsp2\script_build\inspect_v*.log` manually. The SKILL.md
   note "carries the build log in its message" wasn't borne out in my
   run — `e.args[0]` was just the short string. Either inline the log
   into the message or document the on-disk location explicitly.

6. **Visual previews are JPEG-encoded.** A binary mask is
   particularly bad at JPEG — when I dumped the binary VAR to look
   at it, ringing artefacts make a clean `0/255` mask look noisy /
   inverted. For "is my mask polarity right?" debugging, an option to
   request PNG / raw PGM previews per VAR would have made the
   adaptive-threshold question (#feedback below) trivially
   inspectable. Workaround: I added a `stats(cleaned).mean` VAR to
   inspect numerically.

7. **Suspected `adaptiveThreshold` bug or semantic surprise.** With
   the same mathematical recipe that worked in numpy
   (`(mean-pixel) > C`), `xi::ops::adaptiveThreshold(invert(blurred),
   block_r, C)` produced a near-all-white output with mean 254.7.
   The portable fallback in `xi_ops.hpp` reads as
   `(src - mean - C) > 0`, which on the inverted image should match
   the numpy formula. I did not isolate this further — switching to
   an inline lambda made the problem moot. Worth adding a unit test
   that pins the polarity of `adaptiveThreshold` on a synthetic
   gradient + dark spot.
