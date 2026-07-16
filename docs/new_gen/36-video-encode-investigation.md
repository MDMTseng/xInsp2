# 36 — hardware video encode for preview streaming (investigation)

Status: **INVESTIGATION / SPIKE** on `exp/video-encode-bench` (off `polaris2_main`,
2026-07). Offline benches only — no product code changed. Bench scripts in
`tools/perf/video/`. **Verdict: DEFER** (see Conclusion) — turbo-JPEG stays the
sweet spot for CPU-resident RGB frames; hardware video encode only pays off if the
frame pipeline becomes GPU-native.

## Question / priority

Could hardware video encoding (Intel QSV / AMD AMF / ARM) stream previews at lower
cost than the current JPEG-per-frame path? Priority (CT): **minimise CPU; bandwidth
is secondary.** Offline first, then judge whether to wire it to the browser.

## Box / tools

Intel Core **i7-13700H** + **Iris Xe** iGPU (Quick Sync). `ffmpeg` at `C:\dlcv\bin`.
Available HW encoders: `h264_qsv`, `hevc_qsv`, `mjpeg_qsv` (all work); `av1_qsv`
**not supported** on this iGPU (needs Arc/Meteor Lake+). MediaFoundation available
(Windows SDK) — the vendor-agnostic-on-Windows path.

## Findings

### 1. Compression (random circles+squares content, 5 MP, per frame)
| encoder | KB/frame | vs turbo-JPEG (~105 KB) |
|---|---|---|
| h264_qsv (moving) | 5.6 | ~19× smaller |
| h264_qsv (static scene) | 1.6 | ~65× smaller |
| mjpeg_qsv (intra, ≈JPEG) | ~270 | — |
Temporal coding crushes JPEG on bandwidth, most of all for slow/static scenes.

### 2. 20 MP hits H.264's 4096-wide limit
`h264_qsv`/MF H.264 **reject 5120-wide** (`MF_E_INVALIDMEDIATYPE`). Options for 20 MP:
`hevc_qsv` (≤8192, ~26–30 fps) or **tiling** into ≤4096 tiles.

### 3. Tiling 20 MP into 4× (2560×1920) H.264 tiles
4 parallel tiles = **26 fps ≈ single HEVC 25 fps**, i.e. NO throughput gain — the one
iGPU encoder is the shared ceiling; the 2.3× parallel-vs-sequential speedup is only
CPU-side (read/convert) overlap. Tiles cost **~1.8× more bitrate** than one HEVC
stream (no cross-tile prediction + 4× overhead). So tiling is a way to *use H.264 on
>4096 images / cut per-tile latency*, NOT a speed lever. Real throughput scaling needs
multiple encoders (multi-GPU / multi-machine).

### 4. Atlas of small previews (validates the "pack many into one stream" idea)
4×4=16 fixed slots, only K updated per frame, h264_qsv:
| updates/frame | KB/frame |
|---|---|
| 1/16 | 5.3 |
| 4/16 | 14.5 |
| 16/16 | 51.5 |
| (intra/MJPEG, all) | 268 |
**Bitrate tracks CHANGE, not slot count** — 1 slot updating = 5.3 KB regardless of the
other 15. With fixed slot→source binding + long GOP + on-demand P-frames, an idle
system costs ~0; a client joining needs a forced IDR keyframe.

### 5. CPU — the decisive metric (and the deflating result)
- **ffmpeg pipeline CPU is high and scales with streams** — dominated by the RGB→GPU
  upload + per-process framework overhead, NOT the encode. (~38 ms CPU/frame isolated
  at 5 MP; the iGPU encode itself is cheap.)
- **Native MediaFoundation probe** (`mf_enc.cpp`, sysmem NV12 → HW H.264): **4.43 ms
  CPU/frame @5 MP** (0.47 cores at 106 fps flat-out; ~0.13 cores at a real 30 fps).
  Removing ffmpeg's overhead exposed the true floor.
- **But vs turbo-JPEG (~6.5 ms CPU/frame, which INCLUDES its color transform), native HW
  is only ~1.5× cheaper — and the MF number EXCLUDES the RGB→NV12 convert.** Add the
  convert and they roughly tie.

The big CPU win ("encode is free on the GPU") only materialises when the frame is
**already a GPU surface** (camera DMA-to-GPU, or GPU-side inspection). For xInsp's
CPU-produced RGB pool buffers, the color-convert + upload tax is the same order as
turbo-JPEG's whole encode, so it caps the benefit.

## Portability

No single portable zero-copy encode API: Intel **oneVPL**, AMD **AMF**, NVIDIA NVENC,
Windows **MediaFoundation** (vendor-agnostic but Windows-only), Linux **VAAPI**
(vendor-agnostic but Linux-only). If ever pursued, it fits xInsp's model as an
**optional hardware-encode capability PLUGIN with per-platform backends** — the core
stays portable, you add the backend you need.

## Conclusion — DEFER

For CPU-resident RGB frames, hardware video encode buys a **modest ~1.5× CPU** edge over
turbo-JPEG at real preview rates, at the cost of platform-specific code, stream state
(GOP/keyframe), a browser video-decode path, and the 4096/HEVC/tiling constraints.
Turbo-JPEG (portable, stateless, already wired, ~6.5 ms/frame) remains the sweet spot.
**Revisit only if the frame pipeline becomes GPU-native**, at which point the atlas +
fixed-4-channel design here (§3–4) becomes the right shape and the CPU win turns real.

## Reproduce
`tools/perf/video/` — `gen_shapes.py`/`gen_atlas.py` (raw frame sources),
`vbench.sh` (codec sweep), `vcpu.sh` (CPU cores), `tiling.sh` (4-tile),
`mjpeg_bench.sh` (HW MJPEG), `mf_enc.cpp` (native MF CPU probe). Needs ffmpeg + numpy.
