# expose — the script data-out sink

`expose` is the official data-out sink for inspection scripts (the VAR
replacement). A script surfaces output with `xi::expose::send("lane", Record{...})`
or by pushing a sealed pack (`xi::use("expose").push(pack)`); expose keeps the
latest per channel, and for SUBSCRIBED channels pushes each record as ONE atomic
binary `XEX1` frame straight to connected WS clients (webUI / HMI / example
drivers). It owns its own UI (`ui/index.html`) — nothing in core/extension/
ui-components carries expose-specific code.

## Wire frames

Two frame versions share the magic `'XEX1'` + a msgpack body. Both are decoded
by the same three cross-tested decoders: `ui/index.html`, `examples/lib/xex1.py`,
and (goldens) `ui-components/test/xex1-golden.mjs` / `tools/xinsp2_py/tests/`.

- **v1** (default wire) — a Record-shaped display frame:
  `{v:1, channel, seq, json:<str>, images:[{key, jpeg:<bin>}]}`. Images are JPEG
  (host cache). The single source of truth is `src/xex1_encode.hpp`.
- **v3** (opt-in via the `frame_wire_v3` def knob) — the canonical frame dump:
  `{v:3, channel, seq, frame:{key:[tag, value], ...}}`. Every entry carries its
  `XI_PACK_TAG_*` so a loader recovers its type EXACTLY; images inline raw pixels
  as `{w,h,c,px:<bin>}`. The generic pack→v3 walk lives in `src/xex1_pack_dump.hpp`
  (`encode_pack_v3`) and is SHARED with `record_save`, so expose's raw wire frame
  and record_save's on-disk `.xex1` file are BYTE-IDENTICAL for the same pack
  (the memory ≈ wire ≈ disk surface).

## E2 — full-resolution compressed preview (`xi.jpeg.encode`)

On the **v3 WS-SEND path only**, expose can substitute a compressed preview for
raw pixels so a live client gets a full-resolution image at a fraction of the
bandwidth, while disk stays raw.

> **Scope note (doc 31).** This substitution serves the PRODUCT-plane preview
> (packs expose receives as a wired sink) and deliberately still lives inside
> expose — moving it behind the egress policy layer is the deferred
> `TODO(preview-egress)` item. The PUSHED live-UI plane is separate machinery:
> `xi.ui.egress` (policy) → expose's byte-blind `xi.ui.sink` (transport) — see
> `docs/internals/ui-egress.md`.

- expose resolves `xi.jpeg.encode` through `get_interface("xi.cap",1)`
  (per-instance, cached, re-resolve tolerant). When the capability is live and
  the `preview_compress` knob is set, each v3 `xi/image` u8 blob entry ships as
  `[BLOB, {preview:{w,h,c,enc:"jpeg",q,data:<jpeg>}}]` — the verbatim blob bytes
  leave the wire and the nested `preview` map rides instead. Source dims come
  from the blob descriptor (the encode reply has none). (The pre-blob `IMAGE`
  tag is retired — docs/new_gen/30.)
- **Fail-OPEN, never to nothing.** No provider / knob off / a per-image contract
  `$fault` / a zero-length jpeg → that image ships RAW. A codec-wide funnel
  failure (imgcodec ships `on_fault:"refuse"`, so one hard fault quarantines it
  and every later call returns `-3`) flips a **persistent degraded** raw mode:
  one status line + one log per transition, and a THROTTLED re-probe via the
  cheap `available()` gate — no per-frame fault storm.
- **The split is deliberate.** The preview substitution lives in expose's own
  `src/xex1_wire_preview.hpp` (`encode_pack_v3_wire`); the shared
  `src/xex1_pack_dump.hpp` is untouched, so `record_save`/replay bytes and the
  record→replay byte-identity (`record_replay_pack_test`) are unaffected.

### Config (`set_def` / instance `config`)

| key                | type | default | effect                                                    |
|--------------------|------|---------|-----------------------------------------------------------|
| `frame_wire_v3`    | bool | `false` | pack-door emits the canonical v3 wire (else v1)           |
| `preview_compress` | bool | `true`  | E2: substitute a compressed preview on the v3 WS wire     |
| `preview_quality`  | int  | `85`    | JPEG quality (1..100) for previews                        |

`get_def` also reports the live `preview_degraded` flag.

## Files

- `src/expose.cpp` — the sink (Record + `xi.pack@1` door), cap resolution,
  degraded-mode state machine.
- `src/xex1_encode.hpp` — the v1 + v3 byte encoders (golden-pinned).
- `src/xex1_pack_dump.hpp` — the SHARED raw v3 dump walk (expose store/pull +
  record_save).
- `src/xex1_wire_preview.hpp` — the WS-SEND-only preview walk (E2).
- `src/xex1_pack_parse.hpp` / `xex1_pack_load.hpp` — replay-side parse/build.
- `ui/index.html` — the self-contained webUI decoder + renderer.

## Proof

`examples/qa_jpeg_preview/` — preview present with full-res dims (JPEG SOF),
size ≪ raw, dedup (`encodes==1` across two channels), per-image raw fail-open on
a codec `$fault`, and a persistent degraded/raw leg with no imgcodec provider.
