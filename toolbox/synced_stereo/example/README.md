# synced_stereo — example project

Two cameras, **one trigger**. Left and right are not two frames that got
matched up — they are two named entries in a single record that was never
apart.

```
t.pack()  ->  { seq: 7, left: <320x240x1>, right: <320x240x1> }
```

Open the webUI, look at the `stereo` channel, and you get both images side by
side with the tick they came from. There is no pairing window to tune, no
timestamp tolerance, no "left buffer is one deep now" failure mode — because
the source gathers the pair before it ever emits.

**What it shows**

- a source can be a **gathering** source: per tick it paints both images and
  seals them into ONE pack, which the host dispatches as ONE trigger. Multi-
  camera sync needs no bus policy when the frames ride the same container.
- **a record is a bundle**, not a picture: N named images plus values, atomic.
  The script reads `left` and `right` off the same `t.pack()`, and pushes them
  back out on one exposed record, so the UI cannot show you a left from one
  tick beside a right from another.
- a missing side is not "wait for the partner" — there is no partner to wait
  for. Either the whole record arrived or none of it did.
- correlation is checked **from the pixels**, not taken on faith from the
  container. The plugin stamps the tick's `seq` into the first 4 bytes of both
  images *and* paints stripes whose phase derives from `seq` (left vertical,
  right horizontal). The script recovers that phase from both images and
  requires the same answer. A stale buffer or an off-by-one pairing shows up
  here as a phase skew.

```
python tools/run_qa.py example_synced_stereo
```

```
fires=12 verdicts=12 ok=12 ng=0 exposed_frames=12
  first: seq=0 stamps=0/0 phase=0/0 want=0
```

The driver fires 12 ticks and asserts **12** runs and **12** exposed records —
not 24. Two independent streams would give you two of each. It then asserts the
negative half: left and right must be genuinely *different* images (rows
identical on one, columns identical on the other). "Perfectly correlated" is
trivially true if a source hands you the same buffer twice, so a correlation
check on its own would pass for the most broken source imaginable.

**Files**: `project.json` (stereo + expose), `instances/`, `inspect.cpp`,
`driver.py`.

See also `qa/qa_pack_stereo/` — the same correlation proof as a lean regression
test, with no expose leg.
