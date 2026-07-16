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

## Throughput (honest): the chain is WRITER-bound, not producer-bound

On this box the raw TCP loopback ceiling (single sender, 4 MiB chunks, this
recv pattern) is **612 MB/s**; the backend egress sits at **~480 MB/s**. Removing
producer copies did NOT raise MB/s — killing copy 4 (send, ~5 ms) and half of the
encode (~6 ms) left throughput at ~480. The ordered single writer's kernel `::send`
is the ceiling; the producer runs within it. So these changes are about RESOURCE,
not rate: they cut producer CPU and the memory-bandwidth pressure the writer
competes with (which is what nudges 480 toward the 612 ceiling, marginally).

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

**20 MP (5120×3840×3 = 59 MB/frame), after:**

| rate            | MB/s | backend CPU (cores) | note |
|-----------------|------|---------------------|------|
| 20 MP@6         | 352  | 1.56 | sustained clean |
| 20 MP@8         | 443  | 1.88 | near the writer ceiling, sustained |
| 20 MP@30        | —    | —    | byte-cap drops the client (correct slow-consumer protection at an unsustainable 1770 MB/s demand) |

The brief's baseline had 20 MP "~4–5 fps marginal"; post-wave it sustains 6 fps
clean and ~8 fps at the ceiling — here the producer-copy relief DOES lift
sustainable throughput, because at 59 MB/frame the mint+encode copies (~4× the
5 MP cost) were contending hard with the writer and pushing it into the marginal /
byte-cap-drop regime. (20 MP "before" was not separately re-measured on this box;
the 5 MP before/after is the controlled delta.)

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

## Not done (deliberate)

- **Scatter-gather encode** (kill encode's remaining blob→Writer copy, ~5 ms) —
  the machinery exists (`xi.emit@2` multi-segment + a pack retain as the owner),
  but throughput is writer-bound so it would not raise MB/s, and interleaving the
  blob span into the msgpack frame while keeping bytes golden-identical + retaining
  the pack across the async writer is a poor risk/reward vs the CPU already saved.
  Design is straightforward if a future producer bottleneck justifies it.
