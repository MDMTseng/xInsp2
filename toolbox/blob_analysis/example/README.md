# blob_analysis — example project

`min_area` is the noise gate, and `blobs` is **geometry**.

The script paints one scene that has both a part population and a noise
population in it:

```
 3 solid 4x4 squares  -> area 16 each   ("the parts")
12 single pixels      -> area  1 each   ("the noise")
```

and runs blob_analysis's pack door over it **twice**:

```
raw=15 gated=3 gate=10 geom=1 (3 parts + 12 specks drawn)
```

- **RAW** (`min_area: 1`) — 15 blobs. The specks are really in the image.
- **GATED** (`min_area: 10`) — 3 blobs, and their centroids come back at
  (7.5, 7.5), (23.5, 11.5), (37.5, 21.5): dead centre of the three squares.

Drag `min_area_gate` past 16 in the UI and the gated leg drops to **zero** — the
gate will throw your parts away just as cheerfully as your dirt. That is the
thing to feel before you ship a recipe.

**What it shows**

- blob_analysis returns per-blob `area`, `cx`/`cy`, the bounding box and the
  traced `contour` as ONE nested canonical-msgpack entry. `inspect.cpp` contains
  the mp walk that reads it — the ~30 lines every real consumer writes once.
  (Nesting is msgpack's job; a flattened `blob_0_cx` key convention would rot the
  first time a blob grew a field.)
- a parameter that changes per frame rides **in the pack** (`min_area` here);
  `instances/det/instance.json` is only the fallback the door uses when the pack
  doesn't carry one (`in.i64_or(key, def)`).
- a missing or mis-typed `gray` comes back as a normal sealed pack carrying
  `$fault` — checked before the results are read, never a silent zero.

**No camera.** The scene is synthesised in the script so the ground truth is
exact. In a real project `gray` is your frame converted to single-channel u8;
nothing else about the call changes.

**Files**: `project.json` (det + expose), `instances/det/instance.json` (the
def-layer fallback), `inspect.cpp`, `driver.py`.

```
python tools/run_qa.py example_blob_analysis
```

The driver asserts *both* halves — 15 ungated and 3 gated. Checking only the
gated 3 would pass just as happily on an image that never had noise in it, which
would demonstrate nothing.

See also `qa/qa_pack_param_modulation/` (the same door, with `threshold` swept
per frame and the blob count asserted as a staircase) and
`qa/qa_use_pack_door/` (the script-built-pack write half).
