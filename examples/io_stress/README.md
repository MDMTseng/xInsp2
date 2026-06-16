# io_stress — extractor/constructor against a rich contract

A stress test for the typed-I/O wiring (see
[`../../docs/internals/typed-io.md`](../../docs/internals/typed-io.md)),
using a plugin that emits **deterministic fake data** — no image processing.

`synthetic_features` turns a `seed` into a rich output: a feature array (each a
nested `{pose, score, edge}`), plus aggregates (`count`, `centroid`, `best`,
`summary`, `roi`). Its `io.hpp` then has to handle all of it:

- **the whole output is record-class** — every getter returns a nominal type
  (`count()/mean_score() -> xi::Number`, `centroid() -> xi::Point`,
  `roi() -> xi::Roi`, `best_pose() -> xi::Pose`, `feature() -> Feature`), each
  carrying its `src`. Scalars are only read at the leaf with `.value()`/`.x()`.
- **nested records + path reads** — `best_pose()` (from `best.pose`),
  `mean_score()` (path `summary.mean_score`);
- **a typed array** — `features() -> std::vector<Feature>` (lightweight handles);
- **a custom nominal type** — `Feature` (defined in the plugin's `io.hpp`, not the
  core) with `pose()` / `score()` / `edge()`;
- **the input constructor takes record-class values, not scalars** — stage 2's
  input is built straight from stage 1's extracted typed values:
  `build().seed(e.count()).threshold(e.mean_score()).roi(e.roi())`. Each value
  carries its `src`, so the constructor records every field's provenance
  (`in2.prov_of("seed"|"threshold"|"roi") == "syn"`), and the ROI round-trips out;
- **NA** — `process({})` with no `seed` hits `xi::require` → NA, and the
  extractors stay total (`extract(NA).count().value() == 0`, `feature(0).is_na()`).

The whole point: the payload is still schema-less cJSON; the types are only names
in the wiring layer. Run it (no frame needed) or see
`vscode-extension/test/ws_io_stress.test.mjs`.
