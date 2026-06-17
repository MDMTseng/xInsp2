# calc_pipeline — two plugins, no image processing

The smallest "wire two plugins together" example: a pure-number data pipeline.

```
input ──▶ adder0 ( + addend ) ──▶ multiplier0 ( × factor ) ──▶ answer
```

- `plugins/adder` — `result = value + addend`
- `plugins/multiplier` — `result = value * factor`
- `inspect.cpp` pushes the frame counter through both and `VAR`s each stage.

Both are plain C ABI plugins that move only `xi::Record` JSON — there is **no
`xi::Image` anywhere**. `addend` / `factor` come from each instance's config
(`instances/<name>/instance.json`) and round-trip through `get_def`/`set_def`,
so you can retune them live in the instance UI without touching the script.

## Run

**VS Code:** open this folder with the xInsp2 extension → Compile → Run. The
backend compiles the two project plugins from source (`cl.exe`), then the
Variable Window shows `input`, `added`, `product`, `answer` each frame.

**Headless:**

```
backend/build/Release/xinsp-backend.exe --project=examples/calc_pipeline --autostart-fps=2
```

With the defaults (`addend=10`, `factor=3`): `answer = (frame + 10) * 3` —
frame 0 → 30, frame 1 → 33, frame 5 → 45.

## What it shows

- Authoring **data-only plugins** (Record in, Record out; config via get_def/set_def).
- **Chaining plugins** in a script: stage 2 consumes stage 1's output by field
  (`added["result"]` → `multiplier0`).
- **Per-instance config**: two instances feed different numbers into the same
  plugin types.

See [`docs/guides/write-a-plugin.md`](../../docs/guides/write-a-plugin.md) and
[`docs/guides/write-a-script.md`](../../docs/guides/write-a-script.md).
