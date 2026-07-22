# mock_camera — example project

Auto-exposure. The script measures the frame and **drives the camera back**.

The camera boots at `gain: 0.2` — deliberately far too dim. Run it and watch the
first verdict:

```
seq=0  mean=17.3   gain=0.200 -> 1.269
seq=1  mean=111.8  gain=1.269 -> 1.248  [in band]
seq=2  mean=112.3  gain=1.248 -> 1.222  [in band]
```

One frame to converge, then it regulates. Drag the `target_mean` Param in the UI
and it re-converges on the new target.

**What it shows**

- a SOURCE plugin is not one-way: `mock_camera` also has an `xi.pack@1` door, so
  `xi::use("cam").process(ctrl)` pushes a `{command:"set_gain"}` pack straight
  back into it. Analysis → actuation, with nothing but the pack plane.
- the source echoes its own state per frame (`gain` rides on the same pack as
  the pixels), so the control law never has to guess which setting produced the
  frame it is holding — that is what makes it stable while a command is still
  in flight.
- the sealed door output *is* the ack. There is no separate reply channel.
- `xi::result(0, ...)` for "still settling": not a pass, not a defect. A run that
  has no verdict to give should say so rather than pick one.

**Frame latency is the contract.** A commanded gain lands on the *next* emitted
frame. This is frame-rate regulation, not a sub-frame servo.

**Files**: `project.json` (cam + expose), `instances/cam/instance.json` (the dim
start), `inspect.cpp`, `driver.py`.

```
python tools/run_qa.py example_mock_camera
```

The driver asserts *both* halves — that the first frame is out of band and that
the last ones are in it. A demo that only checked the ending would also pass if
the camera had started converged.

See also `qa/qa_pack_feedback/` — the same loop as a regression test, with the
convergence envelope asserted frame by frame.
