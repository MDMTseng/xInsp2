# qa_pack_feedback — closed-loop control at frame latency

The maintainer-settled claim this example proves live: **closed-loop control
(analysis → adjust the source) is expressible TODAY, at frame latency, with
nothing but the existing pack plane** — because a SOURCE plugin can also
expose a pack door. mock_camera emits pack frames *and* accepts control packs
through its own `xi.pack@1` door: the bilingual source, both directions.

## The loop

```
        ┌─────────────────────────────────────────────────┐
        │                (per trigger, one frame)          │
        ▼                                                  │
  mock_camera ──emit pack {frame, seq, gain}──▶ script     │
  (pack mode)                                   1 SENSE    │
        ▲                                       2 ANALYZE  │  mean intensity
        │                                       3 DECIDE   │  cmd = gain·T/mean
        └──── control pack {command:"set_gain", value} ────┘
              xi::use("cam").process(ctrl)   4 ACTUATE
              (the sealed door output is the ack)
```

Per frame the script reads the pack (`t.pack()`: the pixels **plus the gain
the frame was painted with** — the plant echoes its own state), computes the
mean intensity, derives a proportional multiplicative correction toward the
target band (110 ± 14), and pushes a control pack into the camera's own door.
mock_camera clamps, applies, and acks; the new gain is in effect for the
**next** emitted frame.

Observed convergence (initial gain 0.2, target 110 ± 14, 64×48 @ 20 fps):

```
mean:  13.7 → 103.1 → 111.0 → 112.0 → 112.2 → ...   (in band from frame 2 on)
gain:  0.2  → 1.61  → 1.72  → 1.71  → 1.67  → ...   (then regulates the drift)
```

One commanded step lands the plant in the band; the loop then keeps
regulating against the source's own gradient drift (the base image shifts a
few gray levels per frame) and never leaves the band. The driver asserts the
whole envelope: starts unconverged → in band within 8 frames → monotone
approach → stays in band → every ack echoes the command → at least one
frame's echoed gain equals the previous frame's command (the latency contract
observed on the wire). Verdicts ride the run_result plane (`xi::ok`/`xi::ng`);
**zero `xi::Record`**.

## The closed-loop pattern (frame-latency contract)

What the pack plane promises a control loop today:

- **Granularity: one frame.** A control pack pushed while frame *n* is being
  produced is applied no later than frame *n+1*. The door and the capture
  worker share nothing but the (atomic) knob — there is no intra-frame
  synchronization, and none is promised.
- **The ack is the door output.** `use("cam").process(ctrl)` is request-reply:
  the sealed pack that comes back carries the *clamped, applied* value (or a
  `$fault` entry — a value-less `set_gain` faults loud with `missing_input`,
  it never silently no-ops). Fire-and-forget `use("cam").push(ctrl)` works
  too when the loop doesn't need the echo.
- **Control against the echo.** The frame carries the gain it was painted
  with, so the law (`cmd = gain_frame · target/mean_frame`) is computed
  against the plant state that *produced the measurement* — self-consistent
  even when a previous command is still in flight. This is what makes the
  one-frame race benign: commands are absolute, not incremental.
- **Sub-frame loops are out of scope.** The pack plane is a keyed-buffer
  data plane, not a real-time bus; anything that must act *within* a frame
  (line-rate AEC, strobe sync) belongs in the device/plugin itself, below the
  door. That boundary is the plane's philosophy — see
  [docs/new_gen/07-uniform-keyed-buffer-plane.md](../../docs/new_gen/07-uniform-keyed-buffer-plane.md).

## Files

- `inspect.cpp` — the loop: sense → analyze → decide → actuate, ~40 lines.
- `instances/cam/instance.json` — pack mode, 64×48 @ 20 fps, initial gain 0.2
  (deliberately dim so the run *demonstrates* convergence, not a no-op).
- `driver.py` — spawns the backend, runs the project, asserts the convergence
  envelope on the run_result verdicts.

The control surface itself (config `gain`, exchange/door `set_gain`, clamps,
fault shapes) is documented in
[toolbox/mock_camera/README.md](../../toolbox/mock_camera/README.md).

Run: `python qa/qa_pack_feedback/driver.py` (Windows; backend + plugins built),
or `python tools/run_qa.py pack_feedback`.
