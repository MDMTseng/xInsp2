# live_tune — RESULTS

Converged: **10/10 frames at count == 8** after two `recompile_project_plugin` cycles.

## Score table

| frame | iter 0 (erode 8) | iter 1 (erode 1) | iter 2 (erode 5 + bbox-split) |
|------:|:----------------:|:----------------:|:------------------------------:|
| 00 | 4 | 8 | **8** |
| 01 | 4 | 7 | **8** |
| 02 | 1 | 7 | **8** |
| 03 | 1 | 8 | **8** |
| 04 | 6 | 7 | **8** |
| 05 | 2 | 8 | **8** |
| 06 | 3 | 8 | **8** |
| 07 | 5 | 8 | **8** |
| 08 | 5 | 8 | **8** |
| 09 | 3 | 8 | **8** |
| **# at 8** | 0/10 | 7/10 | **10/10** |

## Iteration log

### iter 0 — baseline (broken plugin, `erode(mask, 8)`)
- Plugin compiled at `open_project` time (cold).
- All 10 frames under-count badly: range 1..6, mean ~3.4.
- Cause: r=11 px circles get reduced to r=3 (area ~28) by an 8-radius erosion → fail the `area >= 50` filter and disappear.

### iter 1 — `erode(mask, 8)` → `erode(mask, 1)`
- One-line edit; `recompile_project_plugin("circle_counter")`.
- Reply: `{plugin: "circle_counter", reattached: ["det"], diagnostics: [<1 warning>]}`.
- 7/10 frames hit 8. Three frames (01, 02, 04) still report 7.
- Inspecting the dumped masks (`snapshots/frame_01/run-000168/vars/mask.jpg`, etc.) shows two adjacent circles fused into a single figure-8 region. `generate.py` doesn't reject overlap when placing circles, so a few frames have touching pairs that survive any contrast pre-processing.

### iter 1.5 — manual sweep of erode radius (probing the "fix erosion only" hypothesis)
| radius | counts | #==8 |
|---:|:--|:---:|
| 1 | 8 7 7 8 7 8 8 8 8 8 | 7/10 |
| 2 | same as 1 | 7/10 |
| 3 | same as 1 | 7/10 |
| 4 | same as 1 | 7/10 |
| 5 | 8 7 7 8 8 8 8 8 8 8 | 8/10 |
| 6 | 7 7 4 4 7 5 7 7 8 6 | 1/10 |
| 7 | 5 6 3 4 7 3 5 5 7 5 | 0/10 |

So no erosion radius alone gets all 10. Above ~6, the smaller r=11 circles collapse below the area floor; below 5, the touching pairs stay merged. The plugin hint ("just tune the radius") is necessary but not sufficient for this dataset.

### iter 2 — `erode(mask, 5)` + bbox-aspect splitter
Final plugin keeps the contrast pass and a moderate erosion, then for every region in the area band computes the bounding-box aspect ratio (`hi/lo`) and credits the region as `round(hi/lo)` circles (min 1).

- Single circle (any radius 11..17) post-erode-5 has bbox aspect ~1 → 1.
- Two touching circles produce a bbox roughly 2× longer one way → aspect ~2 → 2.
- All 10 frames now report exactly 8.

## `recompile_project_plugin` shakedown

Deliberately rewrote `xi::Record()` → `xi::Recordd()` mid-run:

| Question | Observed |
|--|--|
| Does the cmd raise on compile failure? | Yes — `ProtocolError("cmd 'recompile_project_plugin' failed: compile failed")`. |
| Do compile errors land in the rsp's `diagnostics` field? | **No.** The `diagnostics` field on a *successful* recompile is populated only with cl.exe warnings (in this build, just `C4535` from xi_seh.hpp). On a *failed* recompile the rsp is an error rsp with no `data` payload, so the cmd-level diagnostics field doesn't carry them. The actual error lines (`C2039 'Recordd': not a member of 'xi'` etc.) arrive via the side `log` channel and are accessible via `c.on_log(...)`. This matches the skill doc ("the full build log arrives separately as a `log` event"). Possibly worth surfacing in the failure rsp too, but not a blocker. |
| Does the previous DLL stay loaded after a failed recompile? | Yes — re-running all 10 frames after the failed recompile produced exactly the iter-1 counts (`8 7 7 8 7 8 8 8 8 8`). No fallback to "no plugin" or "instance dead" state. |
| Does `reattached` list `det` on a successful recompile? | Yes, both iter 1 and iter 2 returned `reattached: ["det"]`. |
| Does instance state persist? | Not testable here — `circle_counter` is stateless across frames; its `process` reads only the current `src` image. The fact that `reattached` lists `det` and runs continued without reinitialising the project gives confidence the instance object is genuinely re-bound to the new DLL rather than dropped+recreated. |

The recompile loop is fast (cl.exe single-DLL build, no script touch, no project reopen). Felt instant compared to a cold `open_project`.

A note about cl.exe output encoding: on this Chinese-locale Windows the build log lands as CP-950 → mojibake when read as UTF-8 (visible in the shakedown's `log_excerpt`). Skill doc mentions setting `VSLANG=1033` on the backend's environment as a workaround. Didn't change it here since it didn't block the work and doing so would mean restarting the backend.

## Impressions vs prior cases

Prior cases I've worked: `circle_counting`, `golden_defect`, `circle_size_buckets`, `blob_tracker`, `trend_monitor`.

| Aspect | This case (live_tune) | Prior cases |
|--|--|--|
| Edit-test loop | **Genuinely tight.** `recompile_project_plugin` round-trips in seconds; no script reload, no instance reset. The "tune a number, see counts change" loop is right there. | Most prior cases were "write inspect.cpp once, sweep params via `set_param`". Plugin source rarely got touched after initial creation. |
| Failure mode | Old DLL keeps serving runs after a failed recompile, so you don't lose your seat in the loop. Discovered the failure path organically (broken `xi::Recordd()`); the protocol behaved well — clear error, clean retry. | Failure modes were mostly "compile_and_load failed, fix .cpp, retry" — very similar shape, but the granularity is the whole script there, vs one plugin here. |
| Visibility into "why" | The mask preview surfaced through `VAR(mask, ...)` was the key debugging tool; without it I would have stayed stuck at 7/10. `dump_run` → JPEG → Read tool inspecting mask.jpg made the touching-circle issue obvious in seconds. | Same workflow used in `circle_size_buckets`; this case's payoff was bigger because the failure mode wasn't a parameter range, it was a structural counting issue. |
| Hint accuracy | Plugin file's hint says "just fix the erosion radius". This is *partly* misleading — no single radius converges 10/10 against this dataset (sweep above), because of touching circles in `generate.py`. I had to add real logic (bbox aspect splitter), not just nudge a number. The agent had to be willing to depart from the hint. | Hints in `circle_size_buckets` and `blob_tracker` were tight and accurate; this one is the first I'd call "underspecified given the data". |
| Diagnostics in rsp | Documented as a return field but in practice carries only warnings (and only on success). For real error reporting, you must subscribe to `log`. Workable, but the SDK could expose a `last_build_log` accessor or include it in the failed rsp. | N/A — prior cases didn't exercise this surface. |
| Headline feature (recompile loop) verdict | **Solid. Best DX of the cases I've done.** Edit a single file, one cmd, instances re-bound transparently, runs continue. The "linchpin for live tuning" framing is earned. | — |

## Bug fixes made

None to the framework. Two doc/test observations worth flagging to the orchestrator (no source changes):

1. The plugin file's leading comment says "Either reduce the erosion radius or drop erosion entirely" — implies a one-knob fix. With the `generate.py` dataset as shipped (no overlap rejection), this isn't sufficient. Either the dataset should reject overlapping placements, or the hint comment should acknowledge that some frames need region-splitting logic to hit 8/8. Suggest tweaking the comment to "Erosion is one piece — depending on dataset, you may also need to handle merged blobs."
2. `recompile_project_plugin`'s rsp-data `diagnostics` field is populated on success but the failed-recompile rsp is bare. Could be a small backend tweak: include the parsed diagnostics on failure too, even when there's no resulting DLL, so callers don't have to maintain a parallel `on_log` capture just to debug a build break. Skipped here since it's a multi-file change beyond the ~20-line ceiling for in-place fixes.

## Files

- `inspect.cpp` — minimal: imread, `xi::use("det")`, emit `count` + `mask`.
- `driver.py` — full convergence flow including the deliberate-break shakedown.
- `plugins/circle_counter/src/plugin.cpp` — final tuned plugin.
- `driver_log.json`, `driver_log.txt` — full per-iteration record.
- `snapshots/frame_01/`, `snapshots/frame_02/` — dumped masks that revealed the touching-circle issue.
