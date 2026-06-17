# calc_pipeline — two plugins, no image processing

A pure-number data pipeline that wires two plugins together — and shows **both
plugin build modes** side by side.

```
input ──▶ adder0 ( + addend ) ──▶ multiplier0 ( × factor ) ──▶ answer
```

- `plugins/adder` — `result = value + addend`. **`build: cmake`** plugin: it owns
  its own `CMakeLists.txt` and links an **external dependency** (`vendor/mathx`,
  a stand-in for a vendor SDK) to do the add.
- `plugins/multiplier` — `result = value * factor`. Plain **source** plugin: the
  backend compiles it with `cl.exe`, hot-reloaded on save.
- `inspect.cpp` pushes the frame counter through both and `VAR`s each stage.

Both move only `xi::Record` JSON — there is **no `xi::Image` anywhere**.
`addend` / `factor` come from each instance's config
(`instances/<name>/instance.json`) and round-trip through `get_def`/`set_def`,
so you can retune them live in the instance UI without touching the script.

## Run

**VS Code:** open this folder with the xInsp2 extension.

1. `multiplier` compiles from source automatically on open.
2. `adder` is a `build: cmake` plugin, so on first open it reports *"not built —
   run Rebuild Plugins"*. Click **🔧 Rebuild Plugins** in the PLUGINS title bar
   (or right-click `adder` → **Rebuild Plugins**) — the backend runs its CMake
   (building + linking `vendor/mathx`) and loads it.
3. Re-open the project (so `adder0` instantiates against the now-built plugin),
   then Compile → Run. The Variable Window shows `input`, `added`, `product`,
   `answer` each frame.

**Headless:** build the cmake plugin once, then autostart:

```
backend/build/Release/xinsp-backend.exe --project=examples/calc_pipeline --autostart-fps=2
```

(Send `rebuild_plugins` once if `adder.dll` isn't built yet.) With the defaults
(`addend=10`, `factor=3`): `answer = (frame + 10) * 3` — frame 0 → 30, 1 → 33,
5 → 45.

## What it shows

- **Two build modes in one project**: `cl.exe` source plugin (`multiplier`) +
  CMake/prebuilt plugin (`adder`) living together in `plugins/`.
- **An external dependency**: `adder/CMakeLists.txt` builds + links
  `vendor/mathx`. Swap `mathx` for a real vendor SDK (headers + `.lib`, ship its
  `.dll` next to `adder.dll`) and the wiring is identical — see
  [`docs/guides/write-a-plugin.md`](../../docs/guides/write-a-plugin.md)
  (*External libraries & CUDA*).
- **Data-only plugins** (Record in/out; config via get_def/set_def) and
  **chaining** them in a script (stage 2 consumes stage 1's `result`).
- **Per-instance config**: two instances feed the same plugin types different numbers.

See also [`docs/guides/write-a-script.md`](../../docs/guides/write-a-script.md).
