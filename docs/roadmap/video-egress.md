# Roadmap — video encoding on the UI-egress plane (H.264 + WebCodecs)

Status: DESIGN SETTLED, IMPLEMENTATION UNSCHEDULED (2026-07-15, CT + review).
Core change required: **zero**. Pull this page when a real consumer arrives —
a multi-camera dashboard, or remote (non-localhost) live viewing where JPEG
bandwidth actually hurts.

## Where it sits (nothing moves)

```
producer process(): ui.push(chan, image)          — unchanged (spec 31)
      ▼
xi.ui.egress: dispatch by per-channel config
      ├── codec:"jpeg"  → xi.jpeg.encode   (today's arm)
      └── codec:"h264"  → xi.video.encode  (NEW cap, stateful)
      ▼
expose xi.ui.sink → WS binary               — unchanged (byte-blind)
      ▼
client: WebCodecs VideoDecoder → canvas     — new ui-components renderer
```

- The encoder is a **cap plugin**, not egress code: egress keeps policy
  (rate, downscale, codec choice), the codec cap keeps encoding — the same
  split as `xi.jpeg.encode`. On Windows, Media Foundation H.264 is built-in
  (no third-party dependency, no GPL — x264 is deliberately avoided).
- This arm is the **first real consumer of the per-channel egress config
  table** (doc 31 deferred item) — land that table with it.
- The three-way rule is untouched: video is LIVE-UI only; the record plane
  stays raw/lossless.

## Hard contracts (settled in review)

1. **Stateful cap sessions.** Unlike jpeg (pure function), the video cap
   holds an encoder session per `$channel` (GOP state, reference frames).
   It is the second stateful cap service after egress itself — the same
   OwnerGuard/teardown discipline applies. Contract needs: session reset on
   reconfig, session destroy on channel eviction / idle timeout, and a
   **force-IDR verb** (below).
2. **`$seq` is the video↔info join key.** The pipeline already stamps it
   end to end (pack, run_result, record). The encode cap MUST tag each
   output chunk with the input frame's id; egress carries it in the wire
   envelope `{channel, id($seq), key, chunk}` AND in the WebCodecs
   `EncodedVideoChunk.timestamp` (the decoder's output `VideoFrame.timestamp`
   carries it through verbatim). Client joins decoded frames with
   product-plane info on `$seq`.
   **RULING — no pixel burn-in.** Embedding the id in the first pixel row
   (broadcast-timecode style) was considered and REJECTED: lossy encode
   requires macroblock-sized luma bits (a visible strip), it desyncs the
   descriptor dims, and the client pays a per-frame canvas readback. The
   whole wire is ours; the timestamp/envelope path is exact and free.
   Reserve burn-in for pipelines that cross an untrusted middle box.
3. **Drop-not-queue stays; IDR heals it.** expose may drop frames by design;
   an H.264 client shows artifacts until the next keyframe. Rule: periodic
   IDR (~1/s) + **force-IDR on every subscribe edge** (a resumed/new viewer
   must start on a keyframe). No jitter buffer, no playout clock.
4. **Need-driven transmission is native.** WebCodecs has NO wall-clock
   contract — feed a chunk whenever, get a frame. Combined with egress's
   existing probe-then-push: no subscriber → encoder idle, zero encode, zero
   bytes; subscribe → force IDR → stream while watched; unsubscribe →
   session idles (destroy after timeout). "Pause" is simply not sending.
   **Boundary: "rewind what I missed" is NOT this plane** — history is the
   record plane's job (or a future ring-buffer sink plugin), never egress.
5. **Zero-latency encoder config, no B-frames.** 1-in-1-out keeps chunk↔frame
   pairing positional and kills pipeline delay. Client decoder likewise
   configured low-latency (`optimizeForLatency`).
6. **Dedup/LRU is bypassed** for the video arm (stateful encode: same input
   ≠ same bytes; nothing to memoize) — same as the raw-passthrough arm.
7. **Fail-open ladder: `h264(hw) → jpeg → raw`.** Probe for a HARDWARE
   encoder; absent (or the cap is down/$faults) → fall back to the jpeg arm,
   exactly the discipline the jpeg→raw fallback already has. Software H.264
   is never auto-selected (see the platform table — it loses to jpeg).

## When it wins (the selection criterion)

Latency is NOT the reason — both chains are <10ms-class on localhost.
The wins are **backend CPU offload** (hardware encode moves ~17ms/frame of
1080p jpeg CPU off the inspection cores) and **bandwidth** (P-frames are
10–20× smaller; decisive for remote viewing / many cameras).

| platform / scenario | verdict |
|---|---|
| Windows workstation (QSV/NVENC), multi-cam or remote | video arm's home turf: CPU + bandwidth double win |
| localhost single-panel tuning (today's usage) | **jpeg stays** — video adds session complexity for nothing |
| no hardware encoder (lean VM, industrial PC) | software H.264 is WORSE than jpeg (CPU + latency) → ladder falls back |
| RPi5 as edge node, LAN | **jpeg** (Pi 5 REMOVED the H.264 hardware encoder; only HEVC hw *decode* exists) |
| RPi5 over constrained uplink (WiFi) | software x264 @720p purely for bandwidth, with an explicit core budget (e.g. 1 core, degrade fps past it) |
| RPi5 as the *viewer* | jpeg — Pi 5 has no H.264 hw decode either; WebCodecs falls to software |

## Client side

WebCodecs `VideoDecoder` in the VS Code webview (Chromium) and any
chromium-based webapp; one new declarative renderer in ui-components
(chunk in → decode → canvas, model identical to the jpeg renderer).
No MSE/HLS/WebRTC machinery.

## Prototype size, when pulled

One medium batch: MF-based `xi.video.encode` cap plugin + the egress arm +
per-channel config table + the renderer + a qa example (subscribe-edge IDR,
$seq join, fallback ladder legs).
