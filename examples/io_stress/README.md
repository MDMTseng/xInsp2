# io_stress — extractor/constructor against a rich contract

A stress test for the typed-I/O wiring (see
[`../../docs/design/io-types-and-na.md`](../../docs/design/io-types-and-na.md)),
using a plugin that emits **deterministic fake data** — no image processing.

`synthetic_features` turns a `seed` into a rich output: a feature array (each a
nested `{pose, score, edge}`), plus aggregates (`count`, `centroid`, `best`,
`summary`, `roi`). Its `io.hpp` then has to handle all of it:

- **nested records** — `centroid().x()`, `best_pose().x()`, `mean_score()`
  (path read `summary.mean_score`);
- **a typed array** — `features() -> std::vector<Feature>` (lightweight handles);
- **a custom nominal type** — `Feature` (defined in the plugin's `io.hpp`, not the
  core) with `pose()` / `score()` / `edge()` accessors;
- **a constructor** with multiple typed inputs — `build().seed(7).threshold(0.3)`;
- **provenance** — the extractor pipes the producing instance's `src` onto every
  typed value it pulls out;
- **NA** — `process({})` with no `seed` hits `xi::require` → NA, and the
  extractors stay total (`extract(NA).count() == 0`, `feature(0).is_na()`).

The whole point: the payload is still schema-less cJSON; the types are only names
in the wiring layer. Run it (no frame needed) or see
`vscode-extension/test/ws_io_stress.test.mjs`.
