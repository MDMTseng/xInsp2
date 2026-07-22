# imgcodec — example project

The capability nobody calls. Open `inspect.cpp` and look for the codec: it is
not there.

`imgcodec` is a **lib plugin** — no data plane, nothing routes to it, it never
emits. It sits beside the pipeline and registers `xi.jpeg.encode` /
`xi.image.decode` with the host. The consumer here is `expose`: on its way out
to the UI it asks the capability plane whether anyone provides
`xi.jpeg.encode`, and if someone does, the socket carries a full-resolution
JPEG instead of the raw pixel plane.

Run it and the driver prints both halves:

```
with codec:    frames=39 stats={'encodes': 1, 'hits': 39, ..., 'registered': True}
no codec:      frames=39 view_status='expose: jpeg preview OFF (raw fallback) - no xi.jpeg.encode provider'
```

**What it shows**

- **the producer never names the provider.** The script pushes pixels. Whether
  they leave the machine compressed is a *deployment* property — which
  providers happen to be loaded — not a property of the inspection.
- **one encode serves everything.** These pixels are byte-identical every tick,
  and imgcodec keys its memo cache on image content, so 39 frames cost
  `encodes: 1` and `hits: 39`. The counter stays at 1 however long you leave it
  running, and it would stay at 1 with ten consumers instead of one.
- **full resolution, not a thumbnail.** The driver reads the JPEG's own SOF
  marker and requires 320×240 — the source dims — at a fraction of the 76 800
  raw bytes.
- **graceful degradation is the whole point.** Delete `codec` and expose flips
  to raw, says so once in its status line, and keeps streaming. An optional
  capability that cannot lose its provider without breaking is not optional.

**Try it live**: with this running, delete the `codec` instance in the UI. The
frames get fatter; nothing stops. (The driver uses a throwaway copy of the
project for that half instead of the delete button, because `remove_instance`
rewrites `project.json` — a test that edits its own fixture only runs once.)

**Files**: `project.json` (codec + view), `instances/codec/instance.json` (the
provider — one instance, wired to nothing), `inspect.cpp`, `driver.py`.

```
python tools/run_qa.py example_imgcodec
```

The driver asserts *both* halves. Only the first would pass on a build whose
capability plane did nothing; only the second would pass on a build with no
codec at all. The claim is the pair.

See also `qa/qa_jpeg_preview/` (the same wire, asserted per-image, including
fail-open on an image the codec refuses) and `qa/qa_cap_imgcodec/` (a purpose-
built consumer plugin calling `xi.jpeg.encode` / `xi.image.decode` directly
through the host funnel, which is what you would write to consume a capability
from your own plugin).
