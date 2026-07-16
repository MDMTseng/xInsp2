# UI egress internals — `xi.ui.egress` + `xi.ui.sink`

**Design record:** [`../new_gen/31-ui-egress-and-plugin-ui.md`](../new_gen/31-ui-egress-and-plugin-ui.md).
This page documents the runtime: the two lib-plugin-provided caps, the threading,
the retained-state discipline, and the fail-open seams. The core gains nothing —
it is all convention + cap-plane composition (doc 31 "the core gains NOTHING").

## The pipeline

```
producer process():  ui.push(chan, image)          ← one line, resolved cap
      │  cap "xi.ui.egress": writes a latest-wins retained SLOT, returns {ok:1}
      ▼                       (never blocks the inspection lane)
ui_egress lib plugin (cap provider; NO graph identity)
      own timer thread @ fps → per channel with FRESH content:
        probe xi.ui.sink (subscribed?) →                    (no sub → drop, zero cost)
        content-keyed LRU dedup → dispatch by descriptor "t" →
        xi.jpeg.encode (cap) → build XEX1 frame → push xi.ui.sink
      ▼  cap "xi.ui.sink": byte-BLIND store latest + emit_binary if subscribed
expose (pure transport): channel subscriptions + WS fan-out, drop-not-queue
```

Both caps ride the funnel (`get_interface("xi.cap.provider",1)` to register,
`get_interface("xi.cap",1)` to call). `xi.ui.egress` is provided by the
`ui_egress` lib plugin; `xi.ui.sink` is provided by `expose` (a data-plane plugin
MAY also provide a cap — cap-plane-consistent, and it keeps ALL policy in egress
while expose stays byte-blind).

## `xi.ui.egress`@1 — the producer-facing push

- **Pack in:** str `$channel` (the UI channel; default `ui/<instance>`), blob
  `image` (an `xi/image` or `xi/jpeg` self-describing blob), or bool `$probe`
  (→ `$versions`).
- **Pack out:** i64 `ok`=1 — written and returned IMMEDIATELY. push RETAINS the
  input pack (`xi.pack@1` retain) into the channel's LATEST-WINS slot (the prior
  unflushed frame is superseded + released) and wakes the flusher. It never
  encodes, never blocks — the producer's lane is untouched.

**Why retain the pack, not a raw image handle.** The slot must keep the blob's
pool buffer alive across the async flush. Retaining the *pack* uses an UNTRACKED
consumer ref (`PackRegistry::retain` = a plain `++rc`), which ANY thread releases
with a plain `--rc` — so the `reinit`/`release_as(0)` creator-tag hazard (the one
the pack-owner-sweep fix addresses) does NOT apply to these refs. Retaining a raw
`xi_image_handle` instead would reintroduce that owner-correctness problem; packs
sidestep it structurally.

## The flusher — own timer thread @ `fps`

One thread, the sole flusher, wakes every `1000/fps` ms (or on a push). Each tick
it snapshots + clears every slot that has FRESH content (empty slots cost
nothing — the "idle channels are literally free" refinement), then per channel:

1. **Subscription gate (probe-then-push).** Call `xi.ui.sink` with just
   `$channel` (a frame-less probe). Unsubscribed → **drop at zero cost**: no
   dedup, no encode. This keeps "no subscriber → zero encode" EXACTLY true (the
   property `qa_ui_egress` pins). Expose absent → the probe reads absent →
   unsubscribed → drop (fail-open: the project's plugin list decides whether live
   UI exists at all).
2. **Dedup.** Content-keyed LRU memo (`lru_max`, default 32). The key is FNV-1a
   over the descriptor + payload, folded with the policy fingerprint (`quality` +
   `downscale_mp`). The `xi.pack@4` `get_blob` door surfaces the descriptor +
   payload but NOT the pool handle, so — exactly as `xi.imgcodec`'s memo does for
   the same ABI reason — content IS the identity (sealed buffers are immutable).
   Every hit is verified against an **identity witness** (a second, independent
   FNV basis over the same bytes + the payload length) so a 64-bit hash collision
   can never serve the WRONG image; a mismatch re-encodes and replaces the entry
   (`stats.dedup_collisions` counts these — expected 0 in any real run). The same
   image pushed to two channels / across ticks **encodes once**;
   `stats.encodes` vs `stats.dedup_hits` is the proof.
3. **Dispatch by descriptor `"t"`** (all config; E1 is SINGLE-GLOBAL — a
   per-channel override table is DEFERRED, see the Config note below):
   - `xi/image` u8 → `xi.jpeg.encode` at `quality` (default 80). Images larger
     than `downscale_mp` megapixels are **box-downscaled** (area-average) to
     ≤~1MP FIRST, INSIDE egress — the codec stays a pure encoder.
   - `xi/jpeg` → pass-through (the payload IS the jpeg).
   - unknown `"t"` (or a non-u8 image) → a **metadata card** (the descriptor
     rides; the UI shows a labelled card).
4. **Build the wire frame** through the SHARED encoder
   (`plugins/expose/src/xex1_encode.hpp`) — the WS-preview arm
   `[BLOB,{preview:{w,h,c,enc,q,data}}]` for a jpeg, or the verbatim
   self-describing buffer for the raw / metadata cases.
5. **Hand to expose** — `xi.ui.sink` with `$channel` + bin `frame`.

**Fail-open at every seam.** No jpeg cap / codec-down (negative funnel rc) /
`$fault` / empty jpeg → RAW fallback (the raw `xi/image` blob rides), mirroring
expose's E2 preview. Nothing ever fails to *nothing*.

## `xi.ui.sink`@1 — expose's byte-blind ingestion

Provided by `expose`. **Pack in:** str `$channel`, optional bin `frame`
(pre-encoded XEX1 bytes). **Pack out:** i64 `subscribed` (1/0), i64 `seen`. With
a frame + a subscriber → `emit_binary` (broadcast; the client filters by
channel), drop-not-queue at the host WS pipe (the RT8 slow-consumer machinery).
Frame absent = a cheap subscription probe. Expose never inspects the bytes — all
encode/dispatch/dedup policy lives in egress. One provider per process (a second
`expose` instance tolerates `ETAKEN` and runs as a normal sink).

## State + teardown

Per-channel slots + the encode LRU hold retained refs. On teardown / `clear` /
reload the flusher is stopped first, then `drain_all_()` releases every retained
pack ref (plain `--rc`) and clears the LRU. The push handler arrives concurrently
from arbitrary producer/dispatch threads; the timer thread is the sole flusher;
all shared state is mutex-guarded, counters are atomics.

## Config (params/def — never ABI)

`ui_egress`: `fps` (30), `quality` (80), `downscale_mp` (2 → ~1MP target),
`lru_max` (32), `encode` (true). **`encode:false` = RAW PASSTHROUGH** — the
"this channel walks raw" knob (doc 31): the flusher ships the pushed
self-describing blob verbatim (no jpeg, no downscale, no LRU), so push AND pull
both see raw, at raw's honest bandwidth/memory cost. `exchange "stats"` →
`{pushes, flushes, encodes, dedup_hits, dedup_collisions, dropped_no_sub,
raw_fallbacks, raw_passthrough, flush_errors, lru_entries, registered}`;
`"clear"` drains.
Producer opt-in is per-producer (e.g. mock_camera's `ui_preview`, default off).

**DEFERRED (E1): single-global config.** `set_def` sets ONE config for the whole
instance; there is no per-channel override table yet. A project that needs
divergent per-channel policy (different fps/quality per channel) runs a separate
`ui_egress` instance per policy today; the override table lands when a real
consumer needs it.

## Tests

- **`plugins/cap_ui_egress_test.cpp`** (ctest unit) drives the WHOLE pipeline in
  process against the three REAL DLLs (ui_egress + imgcodec + expose) through the
  real cap plane, observed only through the `stats` / `get` / `subscribe`
  exchanges. Because a push does NOT force an early flush (the flusher's wake CV
  predicate only fires on shutdown), a low `fps` gives a race-free window between
  flushes, so it pins the timer-driven semantics DETERMINISTICALLY: full-path
  delivery, **no-subscriber → zero encode** (drop at the probe, `encodes`
  unmoved, expose stores nothing), **content dedup** (`encodes` pinned while
  `dedup_hits` climbs), **latest-wins** (a burst collapses to the LAST frame —
  the superseded ones do zero encode work), **LRU eviction** (`lru_entries` caps
  at `lru_max`), and unregister-on-teardown + pack balance.
- **`examples/qa_ui_egress`** (python e2e) proves the wall-clock behaviour the
  ctest cannot: a 30fps `mock_camera` (`ui_preview=on`) is **rate-capped** to the
  egress `fps` at a real WS client (delivered ≪ pushed) carrying a full-res jpeg
  preview; the **no-subscriber** window shows `encodes==0`/`dropped_no_sub>0`; and
  the **cap-absent** phase (no `ui_egress`) leaves the product plane byte-intact
  with nothing on `ui/cam`.
