# Image & I/O reference (`xi::Image`, `xi::imread`, OpenCV interop)

The image type your inspection scripts and plugins pass around, how to load one,
and how to cross to/from OpenCV. (Header: `backend/include/xi/xi_image.hpp`; I/O
in `xi_io.hpp`.)

## `xi::Image`

An owning, refcounted 8-bit image buffer.

| Member | Meaning |
|---|---|
| `width`, `height`, `channels` | dimensions; `channels` is **1 (gray), 3 (RGB), or 4 (RGBA)** |
| `empty()` | true if any dimension/channel is 0 — **the failure sentinel** (see below) |
| `data()` | pointer to packed pixel bytes (`stride() == width*channels`, rows contiguous) |
| `size()`, `stride()` | byte count / bytes-per-row |
| `as_cv_mat()` | **non-owning** `cv::Mat` view over the same bytes (no copy) |

> **Channel order is RGB, not BGR.** A 3-channel `xi::Image` is RGB. OpenCV's
> defaults assume BGR, so if you `imwrite`/`imshow` an `as_cv_mat()` view the
> channels look swapped — `cvtColor(..., COLOR_RGB2BGR)` first. (This is the one
> gotcha that historically only lived in a header comment.)

Constructors:
- `Image(w, h, c)` — fresh zero-filled buffer.
- `Image(w, h, c, const uint8_t* data)` — **copies** `data` into a fresh buffer.
- `Image::create_in_pool(host, w, h, c)` — (plugins) zero-copy pool-backed buffer
  for *producing* an image to return; bytes land straight in the host ImagePool.
- `Image::adopt_pool_handle(host, handle)` — zero-copy view over a host handle.

## Loading: `xi::imread`

```cpp
auto img = xi::imread(xi::current_frame_path());   // in a script
if (img.empty()) { /* load failed — bad path, non-image, 0-byte, corrupt */ }
```

`imread` is **empty-on-failure, never throws**: a missing/garbage/0-byte/non-image
path returns an empty `Image` (`.empty() == true`), so always check rather than
assuming success. (This is why a bad `run` `frame_path` degrades cleanly instead
of crashing — see the robustness notes in [`../design/fe-be-split.md`].)

## OpenCV interop — both directions

**Image → cv::Mat (zero-copy, borrow):** `as_cv_mat()` returns a Mat that points
into the Image's bytes. **The Mat must not outlive the Image.** Use it to run cv
ops without copying:

```cpp
cv::Mat gray;
cv::cvtColor(img.as_cv_mat(), gray, cv::COLOR_RGB2GRAY);
```

**cv::Mat → Image (owning copy):** `xi::from_cv_mat(m)` is the inverse — it copies
a `cv::Mat` into an owning `xi::Image` so you can `VAR`/record/return it safely
regardless of the Mat's lifetime:

```cpp
cv::Mat mask;                       // some intermediate you computed
cv::threshold(gray, mask, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
VAR(mask_img, xi::from_cv_mat(mask));   // surfaces as a preview/var, lifetime-safe
```

- Supports **8-bit** 1/3/4-channel mats. Convert depth first for float/16-bit:
  `m.convertTo(tmp, CV_8U)`.
- A non-continuous (ROI/sub-mat) Mat is `clone()`d so rows are packed.
- Returns an **empty** Image (check `.empty()`) for an empty or non-8-bit input —
  never a malformed image.

Previously you had to hand-roll `xi::Image(m.cols, m.rows, 1, m.data)`, which
silently assumed 1 channel + continuity; `from_cv_mat` handles channels/continuity
and is the supported path.

## What `VAR` can track

`VAR(name, value)` emits `name` to the client. It works for scalars (int/double/
bool), `std::string`, and `xi::Image` (surfaced as a preview). To emit an
intermediate `cv::Mat`, wrap it with `from_cv_mat` first (above). `VAR` declares a
variable named `name` in scope — see the `VAR` redefinition note in
[`../guides/writing-a-script.md`](../guides/writing-a-script.md) (don't reuse a
name that already exists as a local, e.g. a same-named `cv::Mat`).
