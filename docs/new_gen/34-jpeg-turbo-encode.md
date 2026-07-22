# 34 — feat/jpeg-turbo: wire libjpeg-turbo as the default JPEG encoder

Status: **LANDING** on `feat/jpeg-turbo` (branched off `polaris2_main`, 2026-07).
Goal: make the JPEG preview encoder use libjpeg-turbo's direct SIMD path by default
wherever the library is installed, instead of silently falling back to OpenCV's
libjpeg+cvtColor path.

## The finding

`bench_jpeg` (q=85, RGB gradient) on this box, before vs after:

| image            | OpenCV (libjpeg+cvtColor) | libjpeg-turbo (direct RGB) | speedup |
|------------------|---------------------------|----------------------------|---------|
| 1080p (2.07 MP)  | 17.4 ms · 119 MP/s        | **2.86 ms · 725 MP/s**     | **6.1×** |
| 5 MP (2448×2048) | 38.9 ms · 129 MP/s        | **6.46 ms · 776 MP/s**     | **6.0×** |
| 20 MP (5120×3840)| 162 ms · 121 MP/s         | **24.6 ms · 799 MP/s**     | **6.6×** |

Encode ceiling (single-thread, this synthetic workload): 5 MP **26 → 155 fps**,
20 MP **6 → 41 fps**.

## Why OpenCV was 6× slower — "doesn't OpenCV use libjpeg-turbo?"

It may *link* libjpeg-turbo, but linking it ≠ getting its speed:

1. **Extra `cvtColor` RGB→BGR pass.** `cv::imencode` wants BGR; our frames are RGB,
   so the OpenCV path converts the whole image first — a full extra memory pass per
   encode.
2. **imencode wrapper overhead** — `cv::Mat` wrap, param vector, internal buffer copy.
3. **No effective SIMD.** The measured ~120 MP/s is plain-libjpeg territory; turbo's
   SIMD DCT/Huffman gets ~700–800 MP/s. Whatever this OpenCV bundles, the SIMD path
   is not what runs. The `tjCompress` (turbojpeg API) path encodes RGB directly, no
   cvtColor, no Mat wrapper, SIMD on.

The gap is not "linked vs not" — it is **taking turbo's direct SIMD path vs OpenCV's
generic one**.

## The change

The encode path (`xi_jpeg.hpp`) and its turbo implementation (`encode_jpeg_turbo`,
`tjCompress`) and the CMake link plumbing ALL already existed — gated behind
`XINSP2_HAS_TURBOJPEG`, which defaulted **OFF**, so every normal build silently used
OpenCV. This change makes the flag **AUTO-DETECT** (same pattern as `OpenCV_DIR`):

- Installed at `TURBOJPEG_ROOT` (default `C:/libjpeg-turbo64`) or a system path →
  default **ON**.
- Absent → default **OFF**, falls back to OpenCV. **No new hard dependency.**
- Explicit `-DXINSP2_HAS_TURBOJPEG=ON/OFF` still overrides. Sticky in cache, so an
  existing build dir keeps its value — a fresh configure (or a one-time `-D…=ON`)
  picks up the new default.

turbojpeg is owned **entirely by `toolbox/CMakeLists.txt`** — the backend build does
not reference it at all. Post-CUT the JPEG encoder lives in the **imgcodec lib
plugin** (`xi.jpeg.encode`), which the preview egress (`expose` / `ui.egress`) calls,
so the detection + link live where the encoder does. This turns the real preview
encoder from OpenCV to turbo wherever the box has libjpeg-turbo. (`bench_jpeg`, the
JPEG-encoder bench, also lives in the plugins tree for the same reason.)

## Where it sits in the egress picture (doc 32 / 33)

doc 33 measured the WS send path at a ~1.45 GB/s ceiling with multiples of headroom;
the practical limiter for a *compressed* preview is the **encoder**. This change
attacks exactly that wall: 5 MP preview encode drops from ~39 ms to ~6.5 ms
(~26 → ~155 fps of encode headroom), 20 MP from ~162 ms to ~25 ms (~6 → ~41 fps).
Combined with the already-landed zero-copy send, the raw→JPEG→wire chain now has the
encoder off the critical path for any realistic preview rate.

## Caveat

`bench_jpeg` uses a smooth gradient — DCT-friendly, so it encodes faster and smaller
than a real (noisy) machine-vision frame. The 6× *ratio* is representative (it's the
same image both ways); the absolute fps on real imagery will be somewhat lower.

## Reproduce

```
cmake -S toolbox -B toolbox/build            # auto-detects libjpeg-turbo
cmake --build toolbox/build --config Release --target bench_jpeg
./plugins/build/Release/bench_jpeg.exe 40 2448 2048     # 5 MP
```
