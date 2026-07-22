# record_replay — example project

**A recorded run comes back as a real source.** There is no camera in this
project. `recorded/` holds five `.xex1` captures and record_replay feeds them
into the graph one at a time:

```
replay seq=0 src=replay chan=1 psum=1(118674) meta=1 entries=6
replay seq=1 src=replay chan=1 psum=1(92502)  meta=1 entries=6
...
```

What arrives is not a "replayed frame" object with its own API. It is an
ordinary sealed pack on an ordinary trigger — the same shape a live grab
produces, the same script path, the same verdicts. Only `t.primary_source()`
knows the difference. That is what makes a recorded run usable for tuning a
recipe at your desk with no hardware on it.

**The fidelity is exact, not approximate.**

- the reserved `$channel` / `$seq` the sink lifted into the file header are put
  **back** as entries, so a capture round-trips entry-for-entry — no gains, no
  losses (a file that never carried them replays without them, rather than
  gaining a spurious `""`/`0` pair);
- every other entry is rebuilt by its on-wire type tag: the nested `meta` map is
  still a map, the image is still an image;
- the pixels are the recorded pixels. The script re-adds them and compares
  against the `psum` checksum sealed in at record time.

**Ending is part of the contract.** With `loop: false` the source simply stops
emitting once the last file is gone: the pump keeps ticking, nothing more
arrives, and `get_status` reports `position == total`. A replay that quietly
wrapped and re-fed you frame 1 would be much worse than one that stops.

**Pull source, so the script has two branches.** record_replay emits when
something ticks its door, and that emit lands as the *next* trigger:

```
timer run   (t.is_active() == false) -> pump the source once, xi::result(0)
trigger run (t.is_active() == true)  -> a replayed capture; verify it
```

A corrupt, truncated or foreign file is not an exception either: the source
emits a normal sealed pack carrying `$fault`, and its cursor still advances so
one bad file cannot wedge the loop.

**The other end of the loop.** `recorded/*.xex1` were produced by
`toolbox/record_save/example/` — same encoder, same format. To replay what that
example just wrote, point this one at its live output:

```
exchange_instance("replay", {"command":"set_dir",
                             "value":"toolbox/record_save/example/captures"})
exchange_instance("replay", {"command":"rewind"})
```

(`dir` is resolved relative to the **backend's** working directory, which is why
`instances/replay/instance.json` spells it out from the repo root.)

**Files**: `project.json` (replay + expose — no camera), `recorded/` (five
shipped captures, the only fixtures in this example), `inspect.cpp`, `driver.py`.

```
python tools/run_qa.py example_record_replay
```

The driver decodes the shipped files itself and asserts *both* halves — every
file replayed once, in order, with matching checksums; and then that nothing
further arrives after the last one. "Did I get 5 frames?" alone would pass on a
source that wrapped around.

See also `toolbox/record_save/example/` (the recording end) and
`qa/qa_pack_record_replay/` (record → save → replay in one graph, asserted
byte-for-byte).
