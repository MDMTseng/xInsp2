# cache — example project

The hot-param loop: buffer a frame, retune, re-inspect **the same pixels**.

Run it and let `cam` stream. Each run retains its sealed pack in the ring
(a reference — no pixel copy). Now stop the camera and send:

```
exchange_instance("buffer", {"command":"replay_last"})
```

A new run appears with the camera stopped. That run is the replay — and because
the verdict depends on the `bright_limit` Param, changing the Param and
replaying again re-inspects the *identical* frame with the new value. No
re-grab, which is the whole point when the part is already out of the fixture.

**What it shows**

- pack retention: the ring outlives the frame that produced it
- `replay_last` / `replay_all` / `replay_timed` as ordinary exchange commands
- a tunable `xi::Param` whose value changes the answer on fixed input

**Files**: `project.json` (cam + buffer + expose), `inspect.cpp`, `driver.py`
(`python tools/run_qa.py example_cache` — it asserts a replay run arrives with
the camera stopped).
