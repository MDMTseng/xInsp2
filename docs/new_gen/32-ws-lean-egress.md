# 32 — perf/ws-lean: resource-lean WS binary egress

Status: **LANDED** on `perf/ws-lean` (2026-07, branched off `perf/ws-throughput`).
Goal: make the backend→browser binary egress chain RESOURCE-LEAN — eliminate
per-frame copies/allocations/CPU on the hot path — while preserving throughput
and the exact wire bytes. Follows the socket tuning of doc 23 (RT8 async writer).

## The chain and its copies (measured, 5 MP RGB = ~15 MB/frame)

A raw preview frame (`expose` with `preview_compress:false`) travels:

1. **mint** — the script fills pixels and `add_image_blob` COPIES them into the
   pack's pool buffer. ~11 ms/frame. (producer thread)
2. **encode** — `expose` walks the pack and builds the XEX1-v3 frame. This did
   TWO 15 MB copies: the blob into the msgpack `Writer` buffer, then the whole
   body into a fresh `{'X','E','X','1'} + body` vector to prepend the magic.
   ~11.5 ms/frame. (producer thread)
3. **store** — already `shared_ptr`/move (doc: bench5mp store-copy fix). free.
4. **send** — `Server::send_frame` COPIED the whole (ptr,len) frame into a fresh
   `OutFrame` vector. ~5 ms/frame. (producer thread, inside the emit gate)
5. kernel user→kernel→user (writer thread). 6. browser: already zero-copy views.

Steps 1, 2, 4 are all on the PRODUCER thread. Their cost is CPU + memory-bandwidth
pressure, NOT (as first assumed) the throughput ceiling — see "Throughput" below.

## What changed

- **(A) Recycled outbound buffer pool** (`xi_ws_server.hpp`). The writer returns a
  sent frame's payload vector to a small bounded free-list instead of freeing it;
  `send_frame` reuses one of adequate capacity. A >1 MiB block is committed+zero-
  filled on first touch and released to the OS on free, so a per-frame fresh
  vector re-faulted every page every frame. Bounded (`kBufPoolMax=4`, ≥64 KiB);
  cleared on `close_client` (idle memory → 0). Wire/order/drop semantics unchanged.

- **(B) `xi.emit@2` zero-copy owned emit** (`xi_abi.h` + wiring). An ADDITIVE
  interface behind `get_interface` (frozen @1 untouched; layout `static_assert`s +
  `test_abi_freeze`, same discipline as `xi_pack_v4`). `emit_binary_owned(spans,
  n, owner, release)` hands the host BORROWED segments + an ownership token; the
  host sends straight from those bytes and releases the token (in the producer's
  TU — no cross-DLL heap free) after the send/drop/teardown, EXACTLY ONCE.
  `OutFrame` gains borrowed `segs` + a move-only RAII owner-release, so every drop
  path (epoch/byte-cap/`close_client` clear/`stop` backlog) frees the producer's
  bytes structurally (the RT8/L1 drain discipline made structural, not per-site).
  The host forwarder falls back to the copying v1 sink when no owned sink is
  installed (every test host / `host_mock` / headless runner), so correctness is
  universal; the zero-copy win applies where `service_main` wires it. `expose`'s
  hot `process()` path uses `Plugin::emit_binary_owned(shared_ptr)`. Kills copy 4.

- **(B2) Seed 'XEX1' into the encoder buffer** (`xex1_encode.hpp`). `encode_frame_v3`
  now seeds the 4-byte magic INTO the `Writer` and returns its buffer by move,
  instead of building the body then COPYING it into a fresh magic+body vector.
  Bytes identical (goldens green); the shared encoder means `record_save`'s disk
  write benefits too. Kills one of encode's two 15 MB copies.

- **(C) Mint-then-fill producer convention** (`xi_script_pack.hpp`
  `mint_image_blob`). `add_image_blob` fills a separate buffer then COPIES it into
  the pool. `mint_image_blob(key,w,h,c,dt,len, fill)` mints the pool buffer up
  front and hands `fill` a writable 64B-aligned payload to write pixels IN PLACE
  (camera DMA target, decode-into-pool, procedural fill) — no intermediate buffer,
  no copy 1. Order is mint→fill→adopt→drop-mint-ref (frozen at seal). See the
  demo + the honest caveat below.

## Throughput (re-attributed 2026-07, perf/ws-scatter): "~480 MB/s" was a bench timer artifact, not a ceiling

An earlier draft of this section called the chain "writer-bound at ~480 MB/s"
against a "612 MB/s raw-loopback ceiling," and hypothesised the producer chain or
the python drain client as the limiter. A controlled re-attribution (perf/ws-
scatter, same box/build: a blast writer-ceiling probe + a full-system bench run
with BOTH a python and a tight C++ raw drain, with backend-CPU sampling) shows
that attribution was wrong on every count. See the appendix for the full table.

- **~480 is a synthetic-timer artifact, not a throughput wall.** 481 MB/s = 32 fps
  × 15.04 MB. The synthetic `start{fps}` tick is quantized by the Windows ~15.6 ms
  scheduler granularity: requested 40 / 50 / 60 fps ALL round up to the same
  31.2 ms quantum (32 fps) and land on 481 MB/s. Three different requested rates
  yielding identical throughput is the fingerprint of a shared quantization
  boundary, not a bandwidth ceiling.
- **The drain client is NOT the limiter.** A python raw drain and a tight C++ raw
  drain give byte-identical MB/s at every operating point (5 MP 481/481; 20 MP
  fps=10 528/545). If python `recv` were the cap, the C++ loop would beat it — it
  does not; both are pinned by the same upstream limit.
- **The producer chain is NOT the limiter.** Backend process CPU stays **< 1 core
  of 20** at both 5 MP and 20 MP, at every sustained rate — mint + encode + emit
  has large headroom and is nowhere near CPU- or memory-bandwidth-bound.
- **The real wall is the writer/socket.** Push past the timer quantum (fps ≥ 64)
  and throughput climbs — 568 MB/s sustained, then an 853 MB/s burst — until it
  hits the ordered writer's OWNED-path ceiling (**~826–853 MB/s @5 MP, ~739 @20 MP**;
  the COPY `send_binary` path blasts to **~1180 @5 MP / ~886 @20 MP**) and the
  256 MiB byte-cap fires its protective slow-consumer drop. The writer ceiling is
  ~850 / ~1180 MB/s — NOT 480 / 612.

The RESOURCE conclusion is unchanged and stands: removing producer copies did not
change sustainable MB/s (the timer, then the writer wall, cap it either way), but
it roughly HALVED backend CPU at equal throughput — that is the whole value of this
wave, and it is real. What was wrong was the *why* behind the flat MB/s.

## Measured (bench5mp, 5 MP raw drain, backend process CPU on a 20-core box)

| variant                    | MB/s | backend CPU (cores busy) |
|----------------------------|------|--------------------------|
| baseline (perf/ws-throughput) | 481  | 3.75 @60 / 2.81 @30 |
| + pool (A)                 | 482  | ~3.4 @60 / 2.19 @30 |
| + zero-copy emit (B)       | 480  | emit 5.0→0.9 ms/frame |
| + XEX1-seed (B2)           | 481  | encode 11.5→5.2 ms/frame |
| **A+B+B2 (wave)**          | **481** | **~1.8 @60 / 1.56 @30** |

Backend CPU roughly **HALVED** at equal throughput (3.75 → ~1.8 cores at 5 MP@60).
Per-frame producer profiling: encode 11.5→5.2 ms, emit 5.0→0.9 ms.

> Note (re-attribution): the flat **MB/s** column here is the **fps=60 timer plateau
> (~32 fps × 15.04 MB ≈ 481)**, not the writer ceiling — every variant lands on it
> because the synthetic tick is timer-quantized, not because the writer is capped at
> 480. The load-bearing column is CPU. Writer-ceiling and py-vs-cpp-drain data are in
> the appendix.

**20 MP (5120×3840×3 = 59 MB/frame), after:**

| rate            | MB/s | backend CPU (cores) | note |
|-----------------|------|---------------------|------|
| 20 MP@6         | 352  | 1.56 | sustained clean |
| 20 MP@8         | 443  | 1.88 | timer-limited (see note), sustained |
| 20 MP@30        | —    | —    | byte-cap drops the client (correct slow-consumer protection at an unsustainable 1770 MB/s demand) |

> Note (re-attribution): 443 MB/s @8 is **not** "near the writer ceiling" — the
> OWNED-path writer ceiling at 20 MP is **~739 MB/s** (blast probe), and the COPY
> path ~886. 443 is the timer-quantized rate, same as at 5 MP. The re-attribution
> run also measured backend CPU **< 1 core** at 20 MP (fps 6/8/10 → 0.32/0.45/0.51),
> so the "mint+encode copies contending hard with the writer" story below is not
> supported by the CPU data; the sustainable-fps improvement over the brief's
> "~4–5 fps marginal" baseline, if real, is at most modest memory-bandwidth relief,
> not the producer being the bottleneck. See the appendix.

The brief's baseline had 20 MP "~4–5 fps marginal"; post-wave it sustains 6 fps
clean and ~8 fps sustained. (20 MP "before" was not separately re-measured on this
box; the 5 MP before/after is the controlled delta.)

## Mint-then-fill: the honest caveat

`mint_image_blob` eliminates copy 1 ONLY for a producer whose pixels come from an
in-place-writable source (real camera DMA, a decoder writing into the pool, a
procedural fill). For a producer that already holds the pixels in a buffer and
would `memcpy` them, it is COST-NEUTRAL (one copy either way; marginally more
call overhead than `add_image_blob`). The synthetic bench can't demonstrate the
win without the pixel-generation cost dominating — the value is real cameras /
decoders, where there is no separate buffer to copy from. Demo:

```cpp
// A camera/decoder writes straight into the pack's pool buffer — no copy.
b.mint_image_blob("img", W, H, C, "u8", (int64_t)W*H*C,
    [&](uint8_t* dst, int64_t n) { camera.dma_into(dst, n); /* or decode_into(dst) */ });
```

## Hard constraints held

- Wire bytes BYTE-IDENTICAL: XEX1 goldens (`xex1_fixtures`, `xex1_v2_identity`),
  `record_replay_pack_test`, `golden_plugin` all green. The owned scatter path is
  proven byte-identical to the copy path (`test_ws_async_writer` Phase 4).
- Frozen @1 ABI untouched; `xi.emit@2` additive with layout `static_assert`s +
  `test_abi_freeze` pin. `expose` stays byte-blind.
- Ordered-writer contracts (FIFO, enqueue==wire order, drop-not-queue, 256 MiB
  byte-cap, conn-epoch stale-frame drop, SO_SNDTIMEO wedge) unchanged;
  `ws_async_writer` + `ws_teardown_race` + `qa_slow_consumer` green.
- Teardown/drain: owner tokens released on every drop path via `OutFrame`'s RAII
  (Phase 4 proves exactly-once on send AND on a no-client early return).

## Not done (deliberate) — reconfirmed closed 2026-07 (perf/ws-scatter)

- **Scatter-gather encode** (kill encode's remaining blob→Writer copy, ~5 ms @5 MP,
  ~20 ms @20 MP) — the machinery exists (`xi.emit@2` multi-segment + a pack retain
  as the owner). The perf/ws-scatter re-attribution took a second, independent look
  at whether to build it and reached the SAME conclusion as ws-lean, from the
  opposite direction: **the producer chain is not the bottleneck** — it idles at
  **< 1 core of 20** and already outpaces the writer/socket wall (~850 @5 MP /
  ~740 @20 MP OWNED). Killing this copy would cut producer CPU only (already
  minimal after this wave); it would **not** raise MB/s (sustainable rate is capped
  by the writer wall, and the bench's "480" was a timer artifact — see the
  Throughput section). Combined with the high risk (byte-identity of a hand-
  interleaved scatter split at the exact `bin` offset, pack lifetime across the
  async writer, and every drain path releasing the pack ref — UAF territory), this
  is a clear no-go. Design is straightforward if a future producer bottleneck ever
  justifies it; `PackIn` would need a handle accessor (currently private).

## Appendix — bottleneck re-attribution data (perf/ws-scatter, 2026-07)

Box: 20-core. Build: perf/ws-scatter (= polaris2_main, this wave landed). Method:
a **blast** writer-ceiling probe (blast ONE fixed hot frame through `xi::ws::Server`
with an in-process drain — no producer chain) vs a **full-system** bench (backend +
`expose` raw preview, OWNED emit path) drained BOTH ways — a python raw-recv loop
and a tight C++ raw-recv loop — with backend process CPU sampled via psutil
(cores busy = %/100). All drains count raw socket bytes (no WS-frame parse), so
MB/s is directly comparable. Full harness in the ws-scatter scratch area
(`bench_ws_blast.cpp` with the COPY pacing-guard fix `min(8×frame, cap/2)`,
`drain_cpp.cpp`, `bench_fullsys.py`).

**5 MP (2448×2048×3 = 15.04 MB/frame):**

| configuration                                             | MB/s | backend CPU (cores) | limiter |
|-----------------------------------------------------------|------|---------------------|---------|
| Writer ceiling — blast, COPY `send_binary` (8-frame queue) | 1183 | n/a               | writer/socket |
| Writer ceiling — blast, OWNED `send_binary_owned` (3 inflight) | 826 | n/a            | writer/socket (+ shallow pacing) |
| Full-system, PY  drain, fps=45 / 60                       | 481 / 482 | 0.31 / 0.41    | **synthetic timer quantization** |
| Full-system, CPP drain, fps=45 / 60                       | 482 / 481 | 0.44 / 0.52    | **synthetic timer quantization** |
| Full-system, CPP drain, fps=40 / 50                       | 476 / 482 | ~0.5           | same 31.2 ms quantum (32 fps) |
| Full-system, CPP drain, fps=64                            | 568  | 0.57                | timer/writer (37.8 fps sustained) |
| Full-system, CPP drain, fps=120 (2.3 s burst → byte-cap drop) | 853 | 0.93          | writer/socket OWNED wall → drop |

481 MB/s = 32.0 fps × 15.04 MB. fps 40/50/60 all collapse onto 481 because a
requested 16.7–25 ms tick rounds UP to the Windows ~15.6 ms scheduler quantum
(→ 31.2 ms → 32 fps).

**20 MP (5120×3840×3 = 58.98 MB/frame):**

| configuration                                | MB/s | backend CPU (cores) | limiter |
|----------------------------------------------|------|---------------------|---------|
| Writer ceiling — blast, COPY (harness-fix valid) | 886 | n/a             | writer/socket |
| Writer ceiling — blast, OWNED                | 739  | n/a                 | writer/socket |
| Full-system, CPP drain, fps=6 / 8 / 10       | 346 / 412 / 545 | 0.32 / 0.45 / 0.51 | timer/writer |
| Full-system, PY  drain, fps=6 / 8 / 10       | 346 / 437 / 528 | 0.35 / 0.40 / 0.49 | timer/writer |
| Full-system, CPP drain, fps=15 (0.9 s burst → drop) | 553 | 0.74         | writer wall → drop |

**Conclusions:** (1) py == cpp drain everywhere → the drain client is not the
limiter. (2) backend CPU < 1 core of 20 at both resolutions, every sustained rate →
the producer chain is not the limiter. (3) the "480" plateau is the synthetic
timer's 15.6 ms quantization; the real wall is the writer/socket OWNED ceiling
(~850 @5 MP / ~740 @20 MP). This is the hard data behind closing scatter-gather.
