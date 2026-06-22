# Plugin-Dev Friction Log — 2026-06-22 (DM-7..DM-12)

Second-round friction report from a plugin-author pass over
`plugins/ct_shape_based_matching` (a param-heavy vision operator: ~33 tunables,
captured templates, instance UI). Continues the DM-1..DM-6 series — those are
already fixed (`f059b65`, `ccf80b6`, `076bc44`); this is what surfaced next.

Ranked by author time spent, not by code size of the fix.

---

## Summary

| ID | Sev | Theme | One-line |
|----|-----|-------|----------|
| DM-7  | **High** | param SSOT | Same ~33 params hand-spelled in 5 places; drift is silent. |
| DM-8  | **High** | manifest validation | `unknown_config_key` warns on persisted structured state (arrays/objects). |
| DM-9  | Medium | build mode | Missing `build: cmake` fails *silently* (Rebuild just skips). |
| DM-10 | Medium | manifest authoring | No way to seed/sync `manifest.params` — fully manual reverse-engineering. |
| DM-11 | Medium | UI lint not gated | `lint_plugin_ui.mjs` exists but runs in no save/export/cert gate. |
| DM-12 | Low | image convert | `xi::Image ⇄ cv::Mat` + RGB/BGR is hand-rolled per plugin. |

The throughline of DM-7 / DM-8 / DM-10 is **one param list with no single source
of truth**. That's where the next ergonomics pass has the most leverage.

---

## DM-7 (High) — Param single-source-of-truth: ~33 params live in 5 places

For one operator param (e.g. `min_score`, `roi_edge_1d_match`), the author must
edit, by hand and keep in sync:

1. `write_match_cfg` / `read_match_cfg` — C++ `get_def`/`set_def`/`exchange`
   (`ct_shape_based_matching.cpp:427` / `:455`)
2. `write_tcfg` / `read_tcfg` — per-template variant config (`:483` / `:497`)
3. `MATCH_FIELDS` / `MATCH_FLAGS` / `VAR_FIELDS` / `VAR_FLAGS` — UI JS arrays
   (`ui/index.html:452`–`:458`)
4. the `<input>`/`<select>` form controls (`ui/index.html:100`–`:194`)
5. `manifest.params` (`plugin.json`)

Adding or renaming one param = five edits. Nothing checks they agree, so they
drift quietly: a `manifest.params` default can disagree with the C++ struct
default, or a `get_def` key can have no form control, with no signal.

**Suggested fix:** a declarative param table the author writes once (name, type,
default, range, group) that generates — or at least validates against — the C++
(de)serializers, the UI form, and `manifest.params`. Minimum viable version: a
build/cert-time check that the set of `manifest.params` scalar keys ==
the set of scalar keys `get_def()` emits (see DM-10).

## DM-8 (High) — manifest validation has no "persisted but not a tunable" concept

`get_def()` legitimately persists structured state — here a `templates` array of
`{name,width,height,…}`. The validator iterates **every** config key and warns
`unknown_config_key` on anything not in `manifest.params`, with no skip for
non-scalar values:

- `backend/include/xi/xi_pm_parse.hpp:230`–`:241` — `yyjson_obj_foreach` →
  `unknown_config_key` for any key absent from `params`.
- `:308` — existing `TODO(p2-3-extend): structured object/array params`.

Workaround the author is forced into: declare `templates` as a fake
description-only pseudo-param purely to silence the warning. That pollutes the
"tunables" surface that tooling/agents read.

**Suggested fix:** skip validation for config values that are arrays/objects
(scalars only are tunables), **or** honor an explicit `"managed": true` /
`"structured": true` marker on a param declaration that means "persisted state,
not a tunable — don't validate."

## DM-9 (Medium) — Missing `build: cmake` fails silently

`ct_shape_based_matching` shipped without `"build": "cmake"` yet "worked": it
loaded from its committed prebuilt DLL + `cert.json`. The only symptom was
**xInsp2: Rebuild Plugins silently skipping it** — no error, no warning, stale
code runs. A plugin folder that has a `CMakeLists.txt` (or a source `.cpp` that
links anything beyond the backend-supplied set) but `build != cmake` is almost
certainly mis-declared.

**Suggested fix:** an `open_project` warning when a plugin folder contains a
`CMakeLists.txt` and `build` is not `cmake`/`prebuilt` (mirror the existing
"no built DLL — run Rebuild Plugins" warning path).

## DM-10 (Medium) — `manifest.params` authoring is fully manual

Writing the manifest meant reverse-engineering 34 entries (type/default/range)
from `shape_matcher.h` (`MatchConfig`/`ModelConfig`/`TemplateCfg` structs) and
the UI HTML, by hand. There's no scaffolder and no sync.

**Suggested fix (pairs with DM-7):** a `node sdk/.../gen_manifest_params.mjs
<plugin>` that runs the plugin once (via the new `xi_run_plugin` host-mock,
`076bc44`), reads the keys `get_def()` emits, and seeds a `manifest.params`
skeleton (type inferred from the JSON value; author fills range/description).
Re-run in `--check` mode = the DM-7 consistency gate.

## DM-11 (Medium) — UI convention linter isn't wired to any gate

`sdk/testing/lint_plugin_ui.mjs` already checks the `data-param` / `data-action`
convention well — but nothing runs it on save/export/cert. Result: this plugin
has **~30 latent warnings and zero `data-param`**, even though its control `id`s
already match the canonical param names (so the fix is nearly mechanical). The
convention silently rots the moment a UI is hand-edited.

**Suggested fix:** run `lint_plugin_ui.mjs` (warn-only) on **Export Project
Plugin** and/or on save, surfacing findings through the same diagnostics channel
as compiler errors. Flip to `--strict` as a future cert gate.

## DM-12 (Low) — `xi::Image ⇄ cv::Mat` + RGB/BGR is hand-rolled per plugin

The plugin hand-rolls `xi_to_cv` (clone) and `cv_to_xi` (per-row `memcpy` over
`stride()`) (`ct_shape_based_matching.cpp:95` / `:102`), and separately remembers
`cvtColor(RGB2BGR)` before every `imencode` (`:700`, `:762`). The zero-copy path
(`pool_image` + `as_cv_mat`) vs this copy path isn't obvious, and the
BGR-before-encode rule (DM-2) is exactly the kind of thing that should live in
one helper instead of each author's memory.

**Suggested fix:** ship canonical `xi::to_cv(const xi::Image&)` /
`xi::to_image(const cv::Mat&)` helpers in the SDK with the RGB convention baked
in, plus an `xi::encode_preview(image)` that does the RGB→BGR + jpeg/png encode
once and correctly.

---

## Already fixed this round (context)

- **DM-1** webview message contract (`type` overwritten to `status`) — documented.
- **DM-2** RGB→BGR before encode — documented.
- **DM-3** nested/array Record output API (`set(key,Record)`/`push`) — documented.
- **DM-4** `instance.json` shape + config→`set_def` — documented.
- **DM-5** `xi_run_plugin` headless host-mock CLI — shipped (`076bc44`).
- **DM-6** `xinsp2_find_opencv()` reusable macro — shipped (`ccf80b6`).
- **BUG-1** OpenCV wired into `xinsp2_add_plugin_test` — shipped (`f059b65`);
  `ct_shape_based_matching`'s manual test-target workaround has been removed.

### Round-2 resolutions (`e614363`)

- **DM-8** — manifest validator now skips array/object config values (only scalars
  are tunables), so persisted structured state (`templates`) no longer warns
  `unknown_config_key`. No more fake pseudo-params.
- **DM-9** — `open_project` warns when a plugin folder has a `CMakeLists.txt` but
  `build != cmake` (the silent "Rebuild skips it" trap).
- **DM-12** — shipped `<xi/xi_cv.hpp>`: `xi::to_cv` / `xi::to_image`
  (= `from_cv_mat`) / `xi::encode_preview` (RGB→BGR + encode). The copy helpers
  already existed (`as_cv_mat` / `from_cv_mat`) — this was also a discoverability
  gap; `write-a-plugin.md` now points at them.

**Still open (bigger / need design):** DM-7 (param single-source-of-truth),
DM-10 (`gen_manifest_params` scaffolder, pairs with DM-7), DM-11 (gate
`lint_plugin_ui.mjs` on save/export).
