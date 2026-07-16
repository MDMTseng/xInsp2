# 33 — exp/ws-zerocopy-send: raw-frame WS egress ceiling (blast probe)

Status: **EXPERIMENT** on `exp/ws-zerocopy-send` (branched off `polaris2_main`,
2026-07). Not landed; a measurement spike + a reusable bench (`bench_ws_blast`,
built but NOT ctest/perf-gated).

## Question (CT)

> 完全無 memcpy 的 websocket send，raw 5MP / 20MP 最大 fps 是多少?

## Precondition: the zero-copy send path already exists

The "no memcpy on send" path is **already landed** — `xi::ws::Server::send_binary_owned`
(xi.emit@2 / perf/ws-lean, doc 32). It streams the payload straight from the
producer's borrowed bytes; the host copies only the ~10-byte WS header. doc 32 also
evaluated scatter-gather (killing the *encode-side* copy) **twice** and closed it
no-go — the producer chain idles at <1 core of 20; the wall is the writer/socket.

This experiment rebuilds doc 32's **blast probe** (whose harness was scratch and
never landed) to (a) give CT the raw-frame fps ceiling on *this* box and (b) settle
doc 32's one loose end: its blast had OWNED (826 MB/s) *below* COPY (1183) @5 MP,
which it attributed to shallow OWNED pacing (3 inflight) vs COPY (8 frames).

## Method

`backend/tests/bench_ws_blast.cpp`. A real `xi::ws::Server` on an ephemeral loopback
port (poll thread + writer), plus an **in-process Winsock client** that does the
RFC-6455 handshake then drains raw bytes as fast as `recv()` allows — NO WS-frame
parse, the fastest possible consumer. For each frame it blasts for 2.5 s (after 0.5 s
warmup) via each path, measuring **delivered bytes at the client**, in two source
modes: HOT (reuse one buffer, cache-resident) and COLD (cycle K buffers whose total
> 96 MiB so each send reads cache-cold memory — the realistic producer). Both paths
are paced identically by a client-driven backpressure guard =
`min(8×frame, cap/2 = 128 MiB)` — deep enough to saturate the writer, shallow enough
never to trip the server's 256 MiB slow-consumer drop (doc 32's `min(8×frame, cap/2)`
guard). Equal depth for both paths is the whole point: it isolates copy-vs-no-copy
from pipe depth.

## Results (this box, 2-run stable ±5%)

HOT = one reused buffer (stays in L3 across sends). COLD = cycle K distinct buffers,
K×frame > 96 MiB, so every send reads a cache-cold source — which is what a REAL
producer (camera DMA / decoder / procedural fill) always hands over.

| raw frame              | OWNED hot          | OWNED cold (real)   | COPY hot           |
|------------------------|--------------------|---------------------|--------------------|
| 5 MP  mono8 (4.78 MB)  | ~1180 fps 5.6 GB/s | **~320 fps** 1.5 GB/s | ~305 fps 1.45 GB/s |
| 5 MP  RGB   (14.34 MB) | ~101 fps  1.45 GB/s | **~104 fps** 1.49 GB/s | ~93 fps  1.34 GB/s |
| 20 MP mono8 (18.75 MB) | ~78 fps   1.47 GB/s | **~77 fps** 1.45 GB/s | ~70 fps  1.32 GB/s |
| 20 MP RGB   (56.25 MB) | ~26 fps   1.48 GB/s | **~26 fps** 1.46 GB/s | ~23 fps  1.33 GB/s |

## Findings

1. **The one true ceiling is a ~1.4–1.5 GB/s writer/socket wall — for EVERY frame
   size.** fps = ~1.45 GB/s ÷ frame bytes. Cache-cold (the realistic column):
   5 MP RGB ≈ **100 fps**, 20 MP RGB ≈ **26 fps**, 5 MP mono8 ≈ **320 fps**,
   20 MP mono8 ≈ **77 fps**.

2. **"5 MP mono8 = 1180 fps" was a CACHE artifact, not a real number** (answers CT's
   "why is 5 MP mono so much faster"). At 4.78 MB the reused hot buffer stays resident
   in L3, so the whole loopback copy pipeline (send→kernel→recv) runs cache-hot →
   5.6 GB/s. Rotate through 21 distinct buffers so the source is cache-cold (COLD
   column) and it **collapses 5.6 → 1.5 GB/s** — straight onto the wall. The larger
   frames (≥14 MB) never fit in cache, so hot ≈ cold for them (nothing to lose). A
   real camera/decoder ALWAYS produces cache-cold frames, so the COLD column is the
   honest fps; the HOT small-frame number does not occur in production.

3. **Zero-copy ≥ copy everywhere, given equal pipe depth** (OWNED cold ≥ COPY hot).
   This settles doc 32's caveat: its "OWNED 826 < COPY 1183" was a **harness
   inflight-depth artifact** (3 vs 8), NOT the copy being faster. The COPY path also
   carries a per-frame producer-side full-frame memcpy (`acquire_buf_ + insert`); at
   equal depth the no-memcpy path matches or beats it (~1.1× on large frames, and it
   spends none of the producer CPU the copy path burns — doc 32's "~halved backend
   CPU").

## Honest caveat — even the COLD column is an UPPER BOUND, not delivered-to-browser fps

Same-process, raw-byte drain, no encode, no real client. It measures the **send
path's** headroom, not what a browser sees. doc 32's *full-system* run (real drain +
encode) sustained ~480 MB/s (timer-quantized). So the send path is NOT the bottleneck
for any realistic raw-preview rate (5 MP@30, even 20 MP@8) — it has multiples of
headroom, and the no-memcpy path widens it (and halves backend CPU). The practical
limiter is the encode chain + the real client's drain, not the socket write.

## Reproduce

```
cmake --build backend/build --config Release --target bench_ws_blast
./backend/build/Release/bench_ws_blast.exe
```
