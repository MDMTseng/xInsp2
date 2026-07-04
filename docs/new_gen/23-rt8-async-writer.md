# 23 — RT8: ordered async WS writer (slow-consumer head-of-line fix)

Status: **LANDED** on `polaris2/rt8-lane-headofline` (2026-07). Fixes the last
open red-team P2 (doc 21 §P2 / doc 19 backlog RT8): a slow-but-alive WebSocket
consumer stalling the whole ordered inspection lane.

Threat model: **well-behaved clients** — a laggy but honest viewer (slow UI,
MB-scale previews, a saturated laptop link), not a malicious peer. The malformed/
slow-loris surface is handled elsewhere (handshake timeouts, frame caps).

---

## The bug (CONFIRMED, red-team P2 / RT8)

`backend/include/xi/xi_ws_server.hpp` `send_frame` held `tx_mu_` **across a
blocking `::send`**, and it was called from dispatch WORKER threads INSIDE the
ordered-emit critical section (`backend/src/service_inspect.cpp`
`emit_run_outcome_`, between `turn.wait_turn()` and `turn.complete()`).

A slow-but-alive client that keeps its TCP receive window near-full makes each
`::send` drain at the client's rate. Because that `::send` runs under the emit
gate, the gate cursor is pinned too: the whole ordered lane's throughput collapses
to the client's socket drain rate. `SO_SNDTIMEO=1500ms` does not help — a
slow-yet-progressing client never trips it; it only catches a fully-wedged (0-drain)
peer, and even then at a 1.5 s-per-frame cost paid on the lane.

## The fix — one owned async writer thread

Decouple the send from the emit gate with a single, long-lived **ordered writer
thread** owned by the `Server`:

- **`send_frame`** builds the FULL WS frame (header + payload) into ONE owned
  `std::vector<uint8_t>`, enqueues it on an ordered outbound queue under a new
  `out_mu_` + `out_cv_`, notifies, and RETURNS. No `::send` on the worker thread.
  Because it still runs under the emit gate, **enqueue order == gate order == wire
  order**.
- **The writer thread** (spawned in `start()`, joined in `stop()`) drains the
  queue FIFO and does the blocking `::send` — now the ONLY place that blocks on
  the socket. It reuses `tx_mu_` for the actual `::send` + the `client_` fd
  snapshot, so `close_client`'s shutdown-then-CLOSESOCK-under-`tx_mu_`
  fd-reuse-UAF guard (external review 08 finding 2) is unchanged — only now a
  single thread ever blocks there, never the workers.
- **Ordering:** strict single drainer ⇒ FIFO == exact enqueue order. Never
  reorder, never coalesce.

## Backpressure

The queue is bounded by a byte budget `kOutboundHardCapBytes = 64 MiB`. Two
mechanisms drop a client that cannot keep up, both ending in the SAME terminal
outcome (`close_client`) the FE already handles for an `SO_SNDTIMEO` drop:

1. **Slow-but-progressing** (never trips `SO_SNDTIMEO`, but the backlog grows):
   when an enqueue would cross the cap, `::shutdown(fd)` the client, clear the
   queue, request a proactive drop, and return false. We drop the WHOLE client,
   never individual frames — a silently dropped frame desyncs the WS stream and
   hands a monitor a phantom gap; a clean full drop is honest.
2. **Fully-wedged** (0-drain): the writer's `::send` blocks until `SO_SNDTIMEO`
   (1.5 s) fires, then it `::shutdown`s and requests the same proactive drop.

### Proactive-drop signal (`drop_requested_`) — a gap this fix also closed

The send side can only `::shutdown` the socket; it must NOT call `close_client`
itself (that owns the poll-thread-only rx/msg buffers). **On Windows a server that
shuts down its OWN socket does not make its own `select()` readable** (verified
empirically), so a fully-wedged client that never reacts to the FIN would stay
attached forever — `recv` never returns, `close_client` never runs. This was a
latent weakness in the pre-existing `SO_SNDTIMEO` drop path too. The fix: the send
side sets an atomic `drop_requested_`, and `poll()` honors it by running
`close_client()`+`on_close` **on the poll thread** within one poll tick — a
bounded, peer-independent drop. The flag is cleared on every `close_client` so it
can never fire on a subsequently-accepted client.

## Lifecycle

- `start()`: spawn the writer AFTER the listen socket is up. It is the ONLY thread
  the `Server` owns (the poll loop is driven externally by the service main loop).
- `stop()`: set `writer_stop_` under `out_mu_`, `out_cv_.notify_all()`, JOIN the
  writer, THEN `close_client` + close listen. `joinable()` guards a double-join;
  `stop()` is idempotent (called explicitly AND from `~Server()`). The join is
  bounded — a writer mid-`::send` is released by its own `SO_SNDTIMEO` (≤1.5 s) at
  worst, then observes the stop flag and exits.
- `close_client()`: after nulling `client_` (under `tx_mu_`) it clears the queue +
  resets the byte counter (under `out_mu_`) so no frame crosses connections. The
  null-before-clear ordering is load-bearing (see the code comment): a racing
  `send_frame` either has its frame cleared, or re-reads `client_==INVALID` and
  enqueues nothing. A frame the writer already popped is guarded by `tx_mu_`, which
  also serializes accept (client_ publish) and close — so the in-hand frame sees the
  OLD fd or INVALID, never a newly-accepted client's fd.

## Semantic change (documented in code)

`send_frame` now returns **true on ENQUEUE, not on delivery**. A WS send was never
a delivery guarantee — TCP buffering already meant sent ≠ received. It returns
**false only** when there is no client, or the hard-cap drop fired. Cost: one extra
owned buffer copy of the payload per frame (up to ~16 MiB for a max preview),
unavoidable because the caller's staged-sink buffer is transient — a bounded,
memcpy-scale price versus a 1.5 s lane stall.

## Tests

- **Backend unit** — `backend/tests/test_ws_async_writer.cpp`, ctest
  `ws_async_writer`. Deterministic proof against a fully-wedged client: (A)
  DECOUPLING — every `send_binary` call returns in memcpy time (`max_call_ms`
  ~5 ms, bound < 200 ms), never the ~1.5 s of a blocking send; (B) LIVENESS —
  tens of thousands of calls complete in a 1 s window (pre-fix: ~1–3); (C)
  BACKPRESSURE + CLEAN DROP — the wedged client is dropped within one poll tick;
  (D) ORDER — a draining client receives 300 frames in strict FIFO seq.
  **Pre-fix demonstration:** compiling this test with `-DXINSP2_WS_SYNC_SEND_DEMO`
  (a temporary sync-send path, not in the tree) yields `max_call_ms=1505.92`,
  `calls_in_1s=3` — the assertions A/B fail, capturing the exact RT8 stall.

- **End-to-end qa** — `examples/qa_slow_consumer/` (`driver.py`), discovered by
  `tools/run_qa.py`'s `examples/qa_*` glob. A continuous lane (`max_parallel>1`)
  emits modest raw previews via `expose`; a raw slow-draining WS client throttles
  its reads. Asserts: LANE LIVENESS (backend `cmd:metrics` `frames_total` advances
  far faster than the client drains — pre-fix it tracks the drain), WIRE ORDER
  (received XEX1 `$seq` strictly increasing per channel), and CLEAN DROP +
  CONTINUITY (a fully-wedged client is dropped within bounded time; the single-
  client slot frees so a second fast client connects and gets an in-order stream
  while the lane keeps serving).

## Deviations from the design brief

- **`drop_requested_` proactive close** was NOT in the original brief. It proved
  necessary: without it the "wedged client dropped within bounded time" property is
  unachievable on Windows (self-shutdown does not wake the server's own `select`).
  It is small, poll-thread-owned, and closes a pre-existing latent gap. Flagged for
  review.
- Everything else (64 MiB cap value, FIFO single-drainer, send-return-on-enqueue
  semantic, `tx_mu_` reuse) follows the brief as written.
