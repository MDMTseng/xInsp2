# `xi::ops` cheatsheet

Image operators shipped with the framework. Header
[`backend/include/xi/xi_ops.hpp`](../../backend/include/xi/xi_ops.hpp).
Pure C++ + portable; OpenCV / IPP backends are picked transparently
when available (see `docs/testing.md` perf table for the dispatch
order).

Add `using namespace xi::ops;` to your script for the unprefixed
names.

## Color / channel

| Op | Signature | Notes |
|---|---|---|
| `toGray` | `Image toGray(const Image& src)` | RGB / RGBA → 1-channel. Pass-through when src is already gray. |
| `invert` | `Image invert(const Image& src)` | `255 - x` per pixel. |

## Thresholding / contrast

| Op | Signature | Notes |
|---|---|---|
| `threshold` | `Image threshold(const Image& src, int t, int max_val = 255)` | Hard cutoff. Output is binary 0/255 by default. |
| `adaptiveThreshold` | `Image adaptiveThreshold(const Image& src, int block_radius, int C = 0)` | Local-mean threshold. `block_radius` is half the window size; `C` shifts the threshold. Use this when the background isn't uniform. |

## Smoothing

| Op | Signature | Notes |
|---|---|---|
| `boxBlur` | `Image boxBlur(const Image& src, int radius)` | Mean of `(2r+1)²` neighbourhood. Cheap; useful as a fast local-background estimator. |
| `gaussian` | `Image gaussian(const Image& src, int radius)` | Separable Gaussian. Kernel σ ≈ `radius/2`. |

## Edges

| Op | Signature | Notes |
|---|---|---|
| `sobel` | `Image sobel(const Image& src)` | Magnitude of 3×3 Sobel. Works on gray. |
| `canny` | `Image canny(const Image& src, int low_thresh, int high_thresh)` | Standard Canny — gaussian + sobel + non-max suppression + hysteresis. |

## Morphology

| Op | Signature | Notes |
|---|---|---|
| `erode` | `Image erode(const Image& src, int radius = 1)` | Min over `(2r+1)²`. Shrinks bright regions. |
| `dilate` | `Image dilate(const Image& src, int radius = 1)` | Max over `(2r+1)²`. Grows bright regions. |
| `open` | `Image open(const Image& src, int radius = 1)` | erode → dilate. Removes bright speckle. |
| `close` | `Image close(const Image& src, int radius = 1)` | dilate → erode. Fills bright holes. |

## Region / contour

| Op | Signature | Notes |
|---|---|---|
| `findContours` | `std::vector<std::vector<Point>> findContours(const Image& binary)` | Border-only contour walk on a binary image. Each inner vector is one contour's boundary points. |
| `findFilledRegions` | `std::vector<std::vector<Point>> findFilledRegions(const Image& binary)` | Connected-component labelling — returns ALL pixels of each region (not just border). Use this when you want region areas (`region.size()` is pixel count). |
| `countWhiteBlobs` | `int countWhiteBlobs(const Image& binary)` | Just the count, when you don't need the regions themselves. |

## Statistics

| Op | Signature | Notes |
|---|---|---|
| `stats` | `ImageStats stats(const Image& src)` | `{min, max, mean, stddev}` over all pixels. Useful for sanity checks (e.g., `stats(mask).mean` shows what fraction of a binary mask is set, side-stepping the JPEG-of-a-binary-image preview problem). |

## Template matching

| Op | Signature | Notes |
|---|---|---|
| `matchTemplateSSD` | `MatchResult matchTemplateSSD(const Image& src, const Image& templ)` | Sum-of-squared-differences. Returns best-match position + score. |

## Async wrappers

Every op above is wrapped by `ASYNC_WRAP(name)` which generates
`async_<name>(args...)` returning `Future<Image>`. Use for parallel
hypotheses on the same input:

```cpp
auto a = async_gaussian(img, 2);
auto b = async_gaussian(img, 4);
Image small = a;   // implicit await
Image large = b;
```

## Cooperative cancellation

None of the ops poll `xi::cancellation_requested()` today — they run
to completion regardless of the watchdog. If your script runs ops on
very large images in a loop, poll the flag yourself between
iterations (see `examples/cancel_aware_script.cpp`).

## Dispatch order

When OpenCV / IPP / turbojpeg are linked, ops auto-dispatch
**IPP → OpenCV → portable C++**. Selection happens at compile (not
runtime). See `docs/testing.md` "Performance baseline" for the
relative numbers.
