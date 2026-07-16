# tools/perf/video — hardware video-encode investigation harness

Offline benches behind `docs/new_gen/36-video-encode-investigation.md` (verdict:
DEFER hardware video encode; turbo-JPEG is the sweet spot for CPU-resident frames).

Needs `ffmpeg` on PATH + Python `numpy`. Raw frames go to the CWD — run from a
scratch dir (files are multi-GB). `mf_enc.cpp` is Windows/MediaFoundation only:
`cl /O2 /EHsc /std:c++17 mf_enc.cpp /link mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib`.

- `gen_shapes.py W H N OUT` — raw rgb24 of random moving circles/squares.
- `gen_atlas.py W H N OUT COLS ROWS K` — grid of fixed slots, K updated per frame.
- `vbench.sh` — codec sweep (fps, KB/frame, Mbps) over 5MP/20MP, moving/static.
- `vcpu.sh` — CPU cores per encoder (ffmpeg -benchmark).
- `tiling.sh` — 20MP single-HEVC vs 4× H.264 tiles (parallel vs sequential).
- `mjpeg_bench.sh` — hardware MJPEG single + concurrency sweep.
- `mf_enc.cpp` — native MediaFoundation HW H.264 CPU probe (the true floor).
