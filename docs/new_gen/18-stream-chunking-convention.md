# 18 — Streaming via chunking: the `$stream` / `$part` / `$eof` convention

Status: **DECIDED** (maintainer-settled convention; NO host mechanism).
Worked, QA-gated example: `examples/qa_pack_stream/`.
Reserved keys recorded where every other `$`-key lives:
`xi::pack_contract` (`backend/include/xi/xi_pack_contract.hpp` —
`kStream`/`kPart`/`kEof`), prose registry
`contract/canonical-profile-notes.md` §"Pack-shaped fail-loud", authored-facing
table `docs/reference/data-types.md` §"Reserved Pack keys".

---

## 1. The problem

Sealed packs are immutable and ATOMIC: a consumer sees a pack whole or not at
all — no partial visibility, no append, no host splicing (docs 07/15/17;
byte-identity of `push` is contractual). That is exactly right for a frame; it
is the wrong shape for a payload produced over time or too large to hold
whole: a line-scan strip growing row-band by row-band, a clip recording, a
long capture.

The settled answer is NOT a new pack kind, a mutable pack, or host reassembly
machinery. It is a **convention on top of ordinary sealed packs**: the logical
payload travels as a sequence of chunks — each chunk a normal, immutable,
individually-sealed pack on ONE lane — glued together by three reserved keys.
Every existing guarantee (canonical profile at seal, fault propagation,
ordered-sink delivery, byte-identical dump) applies to each chunk unchanged,
because each chunk IS just a pack.

## 2. The key set

Producer-stamped before seal (like `$channel`/`$seq` — the host never stamps a
sealed pack, doc 17 §2). Reserved: these keys stay out of every plugin's
declared schema keyset, same rule as the doc-15 fault/provenance keys.

| Key | Type | Meaning |
|---|---|---|
| `$stream` | i64 | Producer-chosen stream id. All chunks of one logical payload carry the SAME value. Must be unique among streams concurrently in flight on the lane — derive it from `xi::run_id()`, a producer counter, or any producer-owned scheme. |
| `$part` | i64 | Chunk index: 0-based, DENSE, increasing by exactly 1 per chunk. A gap or regression is a protocol fault (§4), never a reorder to tolerate. |
| `$eof` | bool | Present-and-`true` on the LAST chunk only; ABSENT on every other chunk. The only completion signal a stream has. |

Everything else about a chunk is ordinary entries: the payload
(`add_image`/`add_bin`/`add_mp`), placement metadata, and the usual
`$channel`/`$seq` routing/ordering identity per chunk. `$seq` remains
per-chunk arrival correlation (doc 17); `$stream`/`$part` are the
cross-chunk identity — the two are deliberately separate carriers.

### Overlap guidance (spatially chunked payloads)

A feature that straddles a chunk boundary is invisible to both neighbours
unless the chunks overlap. The convention for spatial chunking (the line-scan
strip case, generalizes to any axis):

- **Chunks overlap by at least the maximum extent of any feature that must be
  detected whole.** With that bound, every feature lies entirely inside at
  least one chunk, and the reassembly window is a SINGLE chunk (§3). A smaller
  overlap forces the consumer to buffer `ceil(extent / stride)` chunks and
  stitch pixels — legal, but you bought the copy.
- **Chunks carry their absolute placement and their exclusive ownership
  band** as ordinary entries (the example uses `y0` = absolute first row and
  `own_h` = rows owned exclusively; consecutive chunks start `stride` apart
  and carry `stride + overlap` rows, clipped at the payload end).
- **Exactly-once rule: a consumer reports a feature only from the chunk that
  OWNS the feature's anchor** (its top row / leading coordinate):
  `y0 <= anchor < y0 + own_h`. A feature fully visible in two overlapping
  chunks is detected by both and reported by exactly one — dedup is a local
  predicate, no cross-chunk memory needed.

## 3. Reassembly semantics

- **The CONSUMER buffers. The host never does.** There is no host reassembly,
  no stream registry, no host timeout. A sink/door/script that consumes a
  chunked stream owns a small state machine per `$stream` id: expected next
  `$part`, its bounded window of buffered chunks, and accumulated results.
- **The window is bounded and small by construction.** With overlap ≥ feature
  extent, the window is ONE chunk: detect in the chunk (its overlap rows give
  it the full view across its trailing boundary), filter by the ownership
  band, release the chunk. Nothing ever holds the whole logical payload —
  that is the point of chunking.
- **Out-of-order tolerance: NONE on one lane.** Doc 17's ordered-sink
  contract already delivers pushes on one lane in frame-arrival order
  (envelope-carried, serialized per lane), and within one frame in script
  call order. The convention therefore requires chunks of a stream to be
  emitted in `$part` order on one lane, and the consumer checks
  `$part == next_part` — a mismatch is a **protocol fault** (`stream_gap`),
  not a cue to build a reorder buffer. Interleaving chunks of DIFFERENT
  streams on one lane is fine (the state machine is keyed by `$stream`).

  **REQUIRED LANE CONFIG (doc 25 RT3-C3).** The arrival-order premise above holds
  only when the streaming lane emits in arrival order. The emit gate is armed ONLY
  under `result_order:"arrival"` *with* concurrency, and the shipped DEFAULT is
  `result_order:"completion"`. So a chunk producer whose chunks are separate
  triggers computed by a **multi-worker** lane (`max_parallel>1`) under the default
  completion order delivers its `use(sink).push(chunk)` calls in COMPLETION order —
  out of `$part` order — and the consumer `stream_gap`-aborts **every** stream under
  load. A streaming lane MUST therefore be one of: **`queue_depth:0`** (rendezvous,
  strict single-worker serial — the RB2 clamp makes this the natural safe shape),
  **`max_parallel:1`** (single worker, inherently ordered), or **`result_order:
  "arrival"`** (multi-worker, but the emit gate replays pushes in arrival order).
  Do NOT run a stream over a default multi-worker lane. (Also: this ordering holds
  for the ordered-**sink** `push` path only — a cross-frame reassembler must be a
  SINK, not a `use().process()` door, whose intermediate calls run pre-gate on the
  raw worker thread and are never arrival-ordered — doc 25 RT3-C4.)

## 4. Failure semantics

- **A `$fault` pack mid-stream poisons the WHOLE stream** (doc 15: `$fault`
  is the one poison marker; a fault pack carrying the stream's `$stream` id —
  producer-minted via `ScriptPackBuilder::fault()` / `PackOut::fault`, or a
  door short-circuit propagation — is the producer's abort signal). The
  consumer: abort the stream, DISCARD partial results (a poisoned frame's
  payload is exactly what downstream must not consume), surface the original
  fault reason on its verdict/output, and DROP any later chunk carrying that
  `$stream` id. There is no partial-delivery salvage and no resume.
- **Missing `$eof` → timeout fault.** A stream that stops arriving without
  `$eof` can only be detected by a deadline, and the deadline is
  CONSUMER-OWNED policy (the convention deliberately does not pick a number —
  a line-scan lane and a clip recorder want different budgets). On expiry the
  consumer aborts exactly as above with reason `stream_timeout`.
- **Protocol violations are faults, not repairs:** a `$part` gap or
  regression (`stream_gap`), a chunk after `$eof` (`part_after_eof`), a
  malformed chunk (missing payload/placement entries — `bad_chunk`). Fail
  loud; never guess-stitch.

## 5. What this convention does NOT give

- **Sub-chunk latency.** A chunk becomes visible only when it seals — the
  producer's chunk size IS the latency floor. Need lower latency? Emit
  smaller chunks; there is no partial pack.
- **Cross-chunk zero-copy.** Each chunk owns its buffers. Detection that fits
  inside one chunk (the overlap design above) is copy-free, but any consumer
  that needs a CONTIGUOUS view across chunks must copy into its own buffer.
  No scatter-gather view exists over sealed packs.
- **Host-side reassembly, resume, retransmit, backpressure.** Delivery is
  doc 17's; loss/abort handling is §4's; there is nothing else.
- **Cross-lane ordering.** The convention rides one lane's arrival-order
  contract. A stream split across lanes has no ordering story — don't.

## 6. The worked example (`examples/qa_pack_stream/`)

A 32×64 virtual strip travels as 4 overlapping chunk packs (stride 16,
overlap 4, `y0`/`own_h` placement) built with `ScriptPackBuilder`; an
in-script consumer state machine reassembles within a one-chunk window.
QA-asserted, zero `xi::Record`:

- a 4×4 feature SPANNING the row-16 chunk boundary is found **exactly once**
  (and provably requires the overlap: exclusive-band-only crops never see it
  whole), a second feature fully inside the shared overlap band is deduped by
  the ownership rule (both chunks see it whole; one reports it);
- the stream completes on `$eof` (and only then);
- an injected mid-stream fault pack (`sensor_drop`) aborts the stream with
  the original reason, partial results discarded, stragglers dropped;
- a stream whose `$eof` never arrives aborts `stream_timeout`.
