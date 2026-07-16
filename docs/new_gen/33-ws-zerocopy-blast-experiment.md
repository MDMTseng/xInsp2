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
parse, the fastest possible consumer. For each frame it blasts ONE fixed hot buffer
for 2.5 s (after 0.5 s warmup) via both paths, measuring **delivered bytes at the
client**. Both paths are paced identically by a client-driven backpressure guard =
`min(8×frame, cap/2 = 128 MiB)` — deep enough to saturate the writer, shallow enough
never to trip the server's 256 MiB slow-consumer drop (doc 32's `min(8×frame, cap/2)`
guard). Equal depth for both paths is the whole point: it isolates copy-vs-no-copy
from pipe depth.

## Results (this box, 3-run stable ±5%)

| raw frame              | OWNED (no memcpy)     | COPY (send_binary)   | owned/copy |
|------------------------|-----------------------|----------------------|-----------|
| 5 MP  mono8 (4.78 MB)  | **~1210 fps** 5.8 GB/s | ~300 fps  1.44 GB/s | **4.0×**  |
| 5 MP  RGB   (14.34 MB) | **~101 fps** 1.46 GB/s | ~92 fps   1.33 GB/s | 1.10×     |
| 20 MP mono8 (18.75 MB) | **~76 fps** 1.44 GB/s  | ~70 fps   1.31 GB/s | 1.09×     |
| 20 MP RGB   (56.25 MB) | **~26 fps** 1.45 GB/s  | ~23 fps   1.33 GB/s | 1.12×     |

## Findings

1. **Zero-copy ≥ copy at every size, given equal pipe depth.** This settles doc 32's
   caveat: the earlier "OWNED 826 < COPY 1183" was a **harness inflight-depth
   artifact** (3 vs 8), NOT the copy path being faster. Level the depth and the
   no-memcpy path wins everywhere.

2. **The win scales inversely with frame size — it's the producer-side memcpy.** At
   4.78 MB the COPY path is bottlenecked by its own per-frame `acquire_buf_ + insert`
   (a full-frame memcpy on the producer thread): ~300 fps × 4.78 MB ≈ 1.44 GB/s is
   the copy ceiling, while OWNED (no such copy) runs 4× faster. As frames grow the
   kernel user→kernel copy in `::send` dominates and both paths converge (~1.1×) onto
   a ~**1.4 GB/s writer/socket wall** for frames ≥ ~14 MB.

3. **Direct answer — raw max fps (send-path ceiling, no memcpy):**
   5 MP RGB ≈ **100 fps**, 20 MP RGB ≈ **26 fps**; mono8 ≈ 3× those (5 MP small-frame
   regime hits ~1200 fps). The ~1.4 GB/s plateau is the ceiling; fps = 1.4 GB/s ÷
   frame bytes.

## Honest caveat — this is an UPPER BOUND, not delivered-to-browser fps

Same-process, in-cache, raw-byte drain with no encode and no real client. It measures
the **send path's** headroom, not what a browser sees. doc 32's *full-system* run
(real drain + encode) sustained ~480 MB/s (timer-quantized) with writer ceilings of
739–826 MB/s. So: the send path is NOT the bottleneck for any realistic raw-preview
rate (5 MP@30, even 20 MP@8) — it has multiples of headroom, and the no-memcpy path
widens that headroom (and, per doc 32, halves backend CPU at equal throughput). The
practical limiter remains the encode chain + the real client's drain, not the socket
write.

## Reproduce

```
cmake --build backend/build --config Release --target bench_ws_blast
./backend/build/Release/bench_ws_blast.exe
```
