# fixturing_demo — typed I/O wiring, end to end

The whole story from [`docs/design/io-types-and-na.md`](../../docs/design/io-types-and-na.md)
in one small project: **locate a part, then fit a tool in the part's frame**
(fixturing / pose-alignment), wired with typed extractors/constructors and NA
propagation.

```
loc (blob_centroid_detector)  ──Pose──▶  fit (line_fit)  ──Line──▶
```

## What each piece shows

- **`plugins/blob_centroid_detector/io.hpp`** — an *extractor* that adapts the
  locator's raw schema-less cJSON (`centroids[]`) into the typed vocabulary
  (`xi::Pose`). Total: a missing index or an NA input yields an NA `Pose`.
- **`plugins/line_fit/`** — a plugin whose search line is authored in a baseline
  pose frame (config) and transformed to the current pose at runtime. It
  validates its input at the compute boundary:
  `if (auto na = xi::require(in, {"current"})) return *na;`.
- **`plugins/line_fit/io.hpp`** — a *constructor* (`build().current(pose)`) that
  assembles the typed input, and an *extractor* that reads back a typed
  `xi::Line`.
- **`inspect.cpp`** — the wiring. It's a straight line with no null-checks: if
  the locator finds nothing or a pose is missing, `line_fit` returns NA and the
  NA flows into the typed `Line` on its own.

## Try it

Open the folder (backend auto-compiles both project plugins + the script), then
**Run**. Or run the regression: `vscode-extension/test/ws_fixturing.test.mjs`.

Note: the type names (`pose`, `line`) are just names over a generic Record — no
schema is enforced on the data; they only give the wiring compile-time
connection safety and feed the manifest `kind` (hover / graph / future wiring UI).
