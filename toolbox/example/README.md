# toolbox — integration example

One inspection station, five toolbox plugins. The per-plugin examples each teach
one plugin; this one teaches how they **compose**, which is the part none of them
can show on its own.

```
cam   (mock_camera)   grab
  |
ring  (cache)         retain the sealed pack — stop the line, re-inspect the
  |                   exact frame that failed, without a re-grab
  |
det   (blob_analysis) threshold + contours, through its pack door
  |
[judge]               blob_count vs a live-tunable band
  |
  +--> view  (expose)      every frame + its numbers, to the UI
  +--> saver (record_save) only the FAILURES, as canonical .xex1 on disk
```

**What it shows**

- **The script is the orchestrator.** The five plugins do not know about each
  other and are not wired to each other. The script pulls from one and pushes
  into the next with `xi::use()`. Rerouting the station is an edit to
  `inspect.cpp` — no plugin changes, no graph config.
- **One plane does all of it.** A camera emit, a request/reply into
  blob_analysis, a retention handoff, a file write and a UI push are all the
  same `xi.pack@1` traffic. No side channels.
- **The recipe travels with the request.** `threshold` / `min_area` go *in the
  pack* to `det`, not into its instance config, so two scripts can drive one
  detector instance with different settings.
- **Strict doors are a feature.** `blob_analysis` requires single-channel u8 and
  faults if it does not get it, instead of guessing at a colour conversion it
  has no business choosing. The conversion is the caller's job — and the caller
  is the one who knows whether it wants a weighted luma or just the red channel.
- **Save the failures, not the frames.** Persisting every frame is how you fill
  a disk and never look at any of it. `saver` is gated on the verdict.
- **Losing evidence is its own defect.** If an NG frame cannot be written, the
  run reports `ng(3)` rather than passing quietly.

**Try it**

Run it, then drag `max_blobs` down to 0 in the UI: every frame turns NG and
`.xex1` captures start appearing under `instances/saver/captures/`. Drag it back
up and they stop. Then stop the camera and send `ring` a `replay_last` — you get
another run on the frame already in the ring, so you can retune `threshold`
against the exact pixels that failed.

**Files**: `project.json` (five instances), `inspect.cpp`, `driver.py`.

```
python tools/run_qa.py example_toolbox
```

The driver asserts each plugin actually did its job — including the negative
halves: that no captures exist *while everything passes* (so the writer is
driven by the verdict, not by every frame), and that a replay still produces a
run *with the camera stopped* (so the run can only have come from the ring).
