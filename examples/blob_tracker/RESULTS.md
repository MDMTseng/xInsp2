# RESULTS — blob_tracker

## Score

**PASS.** All 30 frames match ground truth exactly. Final
`total_crossings == 3` (truth: 3). No per-frame lag.

```
idx file           pred truth delta blobs mat prev  ok
------------------------------------------------------------
  0 frame_00.png      0     0     0     5    0    0
  1 frame_01.png      0     0     0     5    5    5
  ...
 16 frame_16.png      1     1     1     5    5    5    ← blob 2 crosses (300→320)
 17 frame_17.png      2     2     1     5    5    5    ← blob 1 crosses (308→326)
 ...
 24 frame_24.png      3     3     1     5    5    5    ← blob 0 crosses (316→328)
 ...
 28 frame_28.png      3     3     0     4    4    5    ← blob 4 leaves frame
 29 frame_29.png      3     3     0     4    4    4    ← blob 4 still gone
------------------------------------------------------------
per-frame match: 30/30
final total predicted: 3    truth: 3
```

Detector parameters used: defaults (`blur=2, block=40, diff_C=15,
close=1, min_area=200, max_area=4000`). No tuning was needed — the
synthetic dataset is well-separated and the centroid extraction was
right on first compile.

`prev=0` on frame 0 because state is empty before the first call;
that's the expected behaviour and the correct reason `delta=0` even
though blob 4 starts at x=360 (already past the gate).

`blobs=4` on frames 28-29 because blob 4 has reached x≥640 (off-frame)
and is no longer detected. No effect on the count.

## What state did

`xi::state()` is the load-bearing piece for this script. The "is this
a crossing?" decision needs `prev_centroids` from frame N-1, so a
stateless inspect would either count nothing or count every right-side
blob every frame.

Concretely, the script:

1. Reads three keys at the top of every call:
   - `prev_centroids` — JSON array of `{x,y,area}` written by the
     previous call. Default empty.
   - `crossings_so_far` — int running total. Default 0.
   - `frame_seq` — int call counter. Default 0.

2. Runs the detector to get `cur` centroids.

3. For each `cur` centroid, finds the closest unmatched `prev` (greedy
   NN within `match_max_dist=60` px). On a match, tests
   `prev.x < gate && cur.x >= gate` and increments
   `crossings_so_far` if so.

4. Writes back updated `frame_seq+1`, `crossings_so_far`, and
   `prev_centroids = cur` for the next call.

The state Record persists naturally across `cmd:run` calls within a
session (it's a static `xi::Record&` inside the loaded DLL — the DLL
stays loaded across runs). Across `compile_and_load` cycles, the host
saves the state JSON before unloading and restores it into the new
DLL. None of that needed special handling from the script.

`set_raw("prev_centroids", build_centroid_array(cur))` was the
clean way to put a JSON array into the Record. There is no
list-of-records or vector-of-records helper, but `cJSON_CreateArray`
+ `cJSON_AddItemToArray` is straightforward and the round-trip
through the host's `xi_script_set_state` preserves array shape
(it falls through to `set_raw` with `cJSON_Duplicate`, line 351 of
xi_script_support.hpp).

## Did XI_STATE_SCHEMA_VERSION help

**No — and it actually wasted my time.** See the friction section
below for the full write-up; the short version: the documented recipe
for overriding the schema version from a script .cpp does not work on
the script compile path, so the value reported to the host stays at 0
no matter what I `#define`. The bumping/state-dropped mechanism is
real and the protocol path works (I read the source), but the user-
side affordance to *trigger* it from a script is broken. I left
`XI_STATE_SCHEMA_VERSION 1` in the source as a forward-looking stamp
plus a comment block pointing at the friction.

The driver dutifully reports
`no state_dropped event (first-ever load OR same schema)` after
`compile_and_load`. With a fresh backend that's the truth (state was
empty so nothing to drop). To genuinely test the path, I would have
needed to load v1, populate state, then load v2 — but with the
override broken, I can't get past v=0 from a script.

## Image pool behaviour across 30 runs

```
final stats:
  image_pool peak high_water:  3
  image_pool final live_now:   0
  image_pool total_created:    120
```

`total_created = 120 = 4 images/frame × 30 frames` — `input`, `mask`,
`cleaned` from each run (the detector-internal `bg`/`blurred` are
local Images that don't enter the host pool because they're not
emitted as VARs). `live_now = 0` between runs means the post-run
release sweep is doing its job. `high_water = 3` is the per-run peak,
not cumulative across 30 runs, which is what we want — confirms no
leak.

Per-frame snapshots (`driver.py --pool`) show this stable across
every frame:
```
  frame 00  pool live=  0  high_water=  3
  frame 01  pool live=  0  high_water=  3
  ...
  frame 29  pool live=  0  high_water=  3
```

So the cumulative-watermark counters are doing exactly what the
docstring promised. This was the most boring part of the run, which
is high praise.

## Friction notes

These are the bits where the SDK / docs / framework made me guess or
work around something. In rough order of pain:

### 1. `XI_STATE_SCHEMA_VERSION` cannot be overridden from a script

`xi_script_support.hpp:370` says:

```cpp
#ifndef XI_STATE_SCHEMA_VERSION
#define XI_STATE_SCHEMA_VERSION 0
#endif
XI_SCRIPT_EXPORT int xi_script_state_schema_version(void) {
    return XI_STATE_SCHEMA_VERSION;
}
```

with a comment that says "User overrides via:
`#define XI_STATE_SCHEMA_VERSION 2; #include <xi/xi.hpp>`".

The script compile driver (`xi_script_compiler.hpp:352`) emits
`/FIxi/xi_script_support.hpp` so the header is force-included
**before** the user TU is parsed. The `#ifndef` guard and the thunk
function body are therefore both committed before any user
`#define` runs. By the time my `inspect.cpp` does
`#define XI_STATE_SCHEMA_VERSION 1` the function is already
`return 0;`. The W4005 "redefinition" is also visible because both
sides defined it (with the same value), which is a tip-off that the
override pathway was never tested end-to-end.

What does work today (none of these are exposed to the user):

- `cl.exe /D XI_STATE_SCHEMA_VERSION=2 …` — but the script compiler
  doesn't read project-level defines.
- `cl.exe /D XI_SCRIPT_NO_DEFAULT_THUNKS …` and write your own thunk —
  same problem.
- Edit `xi_script_compiler.hpp` to emit `/D` flags from a project
  field — code change, not a workaround.

**Suggested fix**: either (a) move the thunk emission to a `_LATE_`
section of the FI'd header that's wrapped in
`#ifdef XI_SCRIPT_END_OF_TU` and instruct users to put a `#include
<xi/xi_script_finalise.hpp>` at the END of inspect.cpp; (b) expose a
`compile_flags` array on `project.json` that lands as `/D ...` on
cl.exe; or (c) just have the user export their own
`xi_script_state_schema_version` function and let the linker pick it
over the weak default — this is what `__attribute__((weak))` does on
gcc/clang; on MSVC the equivalent is `#pragma comment(linker,
"/ALTERNATENAME:...")` or a `selectany` attribute. Any of those
would unbreak this.

This was the headline ask of the task and I literally cannot test it
from the script side. Calling it out hard.

### 2. `load_project` vs `open_project` — same SDK, very different cmds

The Python SDK has both `Client.load_project(path)` and
`Client.open_project(path)`. The docstring says "Alias for
`load_project`; some tooling uses the `open_project` cmd name.
Behaviour is identical from the SDK's perspective."

It is **not** identical:
- `load_project(path)` → `cmd:load_project` → wants `path` to point
  at `project.json`. Reads the file. Does NOT compile project-local
  plugins. Does NOT instantiate from `instances/`. Returns `ok` even
  if neither happened.
- `open_project(path)` → `cmd:open_project` → wants `path` to point
  at the project FOLDER (or `folder` arg). Compiles project-local
  plugins under cl.exe. Walks `instances/` and builds runtime
  instances. This is what every example actually needs.

I lost ~10 minutes on this: my driver called `load_project`, got
silent success, then `list_plugins` showed zero project plugins
loaded and `list_instances` was empty. No error, no warning log.
First `c.run()` after compile_and_load timed out because `xi::use("det")`
couldn't resolve `det`.

The SDK docstring at `client.py:132-135` actively claims the two are
aliases. They are not. Either the SDK should make them genuine
aliases (call the same backend command), or the docstring should
say "if you want plugins to actually load, call `open_project`."

### 3. `VAR(name, expr)` shadows local-variable names

`VAR(frame_seq, frame_seq_v)` works; `VAR(frame_seq, frame_seq)` is
a redeclaration. The macro literally expands to `auto frame_seq =
frame_seq;`. Comment in `circle_counting/inspect.cpp` already flags
this ("redeclaration trap") so it's clearly bitten before. A small
lint or a `_v` convention for state-mirroring locals would help —
that's what I ended up adopting in this script.

### 4. `xi::Json` has no `release()` / no way to give ownership to Record

To stash an array I had to do
`out.set_raw("centroids", cJSON_Duplicate(arr.raw(), 1))` — i.e. dup
the cJSON tree and let `arr` (the RAII-owning xi::Json) free its
copy. A `Json::release_root()` method that hands the cJSON* over and
nulls the wrapper would save one allocation per emit. Not painful, but
felt unidiomatic.

### 5. Backend crashes on second `open_project` in same session

Cosmetic to this task (driver only opens once) but worth flagging:
running `python driver.py` twice in a row killed the backend with
exit 139 (segfault). First run is fine; backend dies during second
`open_project`. Probably the project-plugin DLL free path stepping
on a still-live handle. Reproducible if it helps — start backend,
run driver, run driver. The first run is what's in the score table
above; the second triggered the crash.

### 6. `state_dropped` event is generally hard to capture from the SDK

`Client.run()` calls `_drain(self._inbox_events)` at the start of
each call (`client.py:240`), which means events emitted AFTER
`compile_and_load` and BEFORE the next `run` get drained and lost
unless the caller polls `_inbox_events` directly. I had to reach
into `c._inbox_events.get_nowait()` (private API) to read the
event after compile. A `compile_and_load` reply that *includes* any
events the load triggered — same way `run()` returns events — would
be cleaner.

## What worked smoothly

- `xi::imread(xi::current_frame_path())` — exactly as documented,
  zero friction. The host-mediated decode keeps the script DLL slim.
- `xi::use("det")` resolved cleanly once `open_project` had been
  called (with the right command).
- `image_pool_stats` cumulative counters are gold. `live_now` going
  to 0 between runs is the obvious "no leak" signal; the
  per-frame snapshot makes it trivial to spot a regression.
- `recompile_project_plugin` not needed for this run, but it's nice
  knowing the plugin can be hot-rebuilt without dropping state.
- The `manifest` block landed cleanly in `list_plugins`. UI sliders
  match the same defaults a project tuning agent would discover.

## Files

- `inspect.cpp` — script (orchestration + tracker + state).
- `plugins/blob_centroid_detector/src/plugin.cpp` — image-math.
- `plugins/blob_centroid_detector/plugin.json` — manifest + UI flag.
- `plugins/blob_centroid_detector/ui/index.html` — slider UI.
- `instances/det/instance.json` — instance config (default tunables).
- `project.json` — project root.
- `driver.py` — opens project, runs 30 frames, scores.
- `PLAN.md` — design rationale.
- `RESULTS.md` — this file.
