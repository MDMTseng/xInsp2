# ui_egress — example project

The live view is not your problem.

Every vision app eventually grows the same bug: the operator's screen ends up
on the inspection path. A 30fps camera encodes 30 JPEGs a second whether or not
anybody is looking, and one slow WS client backs pressure into the thing that
is supposed to be measuring parts.

`ui_egress` is a **lib plugin** that takes the job away. No data plane; it
registers one capability, `xi.ui.egress`, and the whole contract is *write a
latest-wins retained slot for this channel and return*. Encoding and fan-out
happen afterwards, on the plugin's own timer thread, at the UI's rate — never
the producer's.

Run it:

```
unwatched: {'pushes': 60, 'flushes': 10, 'encodes': 0, 'dropped_no_sub': 10, ...}
watched:   delivered=19 in 4.0s  pushed=120  encoded=20
no egress: product=118 live=0
```

**What it shows**

- **nobody watching costs nothing.** 60 frames pushed, 0 encoded. Egress probes
  the sink, sees no subscriber, and drops at the probe. A live view nobody is
  looking at has to be free or it is not a feature, it is a tax.
- **the UI rate wins.** 120 pushes in, 19 frames out: `fps: 5` on the egress
  instance, not `fps: 30` on the camera. The 101 frames in between were
  *overwritten*, not queued — a live view is a view of NOW, so a stale frame
  has no value worth buffering, and the producer never waits for the consumer.
- **the producer integration is one line.** Look for the live view in
  `inspect.cpp`: it is not there. `ui_preview: true` in `instances/cam` is the
  entire wiring; mock_camera pushes each painted frame to the capability from
  inside its own capture loop. The script measures parts and never learns a UI
  exists.
- **no egress, no problem.** Delete the provider and the camera's push becomes
  a silent no-op — `live=0`, nothing on `ui/cam` at all — while the product
  plane keeps running with strictly increasing seq and full-size frames. The
  live view degrades to *absent*, which is exactly what it should degrade to.

Two channels are in play and it is worth keeping them straight: `cam` is the
product plane, pushed by the script; `ui/cam` is the live plane, pushed by the
camera. Different code, different thread, different rate. They meet only inside
`view`.

**Files**: `project.json` (cam + egress + codec + view), `instances/cam`
(`fps: 30`, `ui_preview: true`), `instances/egress` (`fps: 5` — change it and
watch the delivered count move), `inspect.cpp`, `driver.py`.

```
python tools/run_qa.py example_ui_egress
```

`codec` is here because egress delegates its JPEG work to `xi.jpeg.encode`
(see `toolbox/imgcodec/example`). Remove it and egress falls open to raw
pixels rather than to nothing — a third degradation step, asserted in
`qa/qa_ui_egress/`, which also pins the deterministic slot/LRU semantics.
