# RESULTS — circle_size_buckets

- frames scored: **15**
- perfect frames (all 3 buckets exact): **15/15**
- per-bucket total |pred-truth|: small=0, medium=0, large=0
- elapsed: 0.9s
- targets: ≥12/15 perfect AND each bucket ≤5 → **PASS**

## Per-frame

| frame | small t/p/e | medium t/p/e | large t/p/e | total | rej_small | rej_big |
|---|---|---|---|---|---|---|
| frame_00.png | 4/4/0 | 2/2/0 | 5/5/0 | 31 | 20 | 0 |
| frame_01.png | 2/2/0 | 2/2/0 | 4/4/0 | 37 | 29 | 0 |
| frame_02.png | 3/3/0 | 4/4/0 | 1/1/0 | 38 | 30 | 0 |
| frame_03.png | 2/2/0 | 3/3/0 | 4/4/0 | 45 | 36 | 0 |
| frame_04.png | 4/4/0 | 6/6/0 | 1/1/0 | 48 | 37 | 0 |
| frame_05.png | 4/4/0 | 4/4/0 | 4/4/0 | 29 | 17 | 0 |
| frame_06.png | 1/1/0 | 4/4/0 | 5/5/0 | 35 | 25 | 0 |
| frame_07.png | 5/5/0 | 4/4/0 | 2/2/0 | 46 | 35 | 0 |
| frame_08.png | 4/4/0 | 2/2/0 | 4/4/0 | 51 | 41 | 0 |
| frame_09.png | 1/1/0 | 4/4/0 | 7/7/0 | 44 | 32 | 0 |
| frame_10.png | 4/4/0 | 3/3/0 | 5/5/0 | 37 | 25 | 0 |
| frame_11.png | 5/5/0 | 2/2/0 | 3/3/0 | 46 | 36 | 0 |
| frame_12.png | 3/3/0 | 2/2/0 | 4/4/0 | 52 | 43 | 0 |
| frame_13.png | 6/6/0 | 4/4/0 | 2/2/0 | 44 | 32 | 0 |
| frame_14.png | 5/5/0 | 2/2/0 | 3/3/0 | 50 | 40 | 0 |

## New tools shakedown

### Manifest discovery via cmd:list_plugins
- `size_bucket_counter`: manifest present; params = ['close_radius', 'min_area', 'small_max', 'medium_max', 'max_area']
- `local_contrast_detector`: manifest present; params = ['blur_radius', 'block_radius', 'diff_C', 'polarity']

### image_pool_stats

Midpoint (after frame 7):
```json
{
  "total": {
    "handles": 0,
    "bytes": 0
  },
  "by_owner": []
}
```
End-of-run:
```json
{
  "total": {
    "handles": 0,
    "bytes": 0
  },
  "by_owner": []
}
```

### recent_errors

Midpoint:
```json
[]
```
End-of-run:
```json
[]
```

## Friction report

### Did `xi::imread` + `current_frame_path` drop the PNG-source-plugin pattern?

Yes. The previous `circle_counting` example needed a project-local
`png_frame_source` plugin (with `frames_dir` config + `idx` param) to
load frames; this project doesn't have one. The first run was
`c.run(frame_path=str(absolute_png))` driving the script's
`xi::imread(xi::current_frame_path())` and it worked first try. One
file fewer per project, no pattern fiddling, and the path discipline
is now per-call instead of baked into instance.json.

Small concrete win: I no longer have to keep `instances/src/` in sync
with the on-disk frames directory; the driver just enumerates
`ground_truth.json` and passes absolute paths.

### Did the manifest discovery actually help?

Useful for self-verification (the driver round-trips the plugin's
manifest and prints param names from `cmd:list_plugins`), and
authoring `plugin.json` was a copy-and-extend from
`local_contrast_detector` so the cost was zero. Whether downstream
agents *use* it is what matters; from the producer side, writing one
was painless.

One mild surprise: `cmd:list_plugins` returns the manifest fine, but
`cmd:open_project` reply also lists plugins and I didn't check
whether *that* reply carries the manifest. The driver did the
post-open `list_plugins` call as a safety net.

### Did I hit the VAR redeclaration trap on first try?

No, because I'd just read `docs/guides/writing-a-script.md`. But —
the hint paragraph there is what saved me. I wrote
`auto bk = bucket.process(...)` deliberately because I knew
`VAR(bucket, …)` would have shadowed the `xi::use("bucket")` proxy. A
fresh author would still hit it; the doc page already calls this out
explicitly though, so the friction is documentation→behaviour, which
is acceptable.

### Did `recent_errors` capture anything useful?

Empty in both polls — no side-channel errors leaked past any rsp
during the 15-frame run. Reassuring as a "clean run" signal, but a
bit unsatisfying as a stress test of the tool because nothing went
wrong. To exercise it I'd need to deliberately mis-issue a command
(e.g. `c.run(frame_path="does/not/exist.png")`). Recommend the docs
include one canned negative-path example.

### Did `image_pool_stats` show meaningful per-instance footprint?

It returned `{"total": {"handles": 0, "bytes": 0}, "by_owner": []}`
**at both midpoint and end-of-run**. That's the no-leak signal one
hopes for, but it's also suspicious: the script produces several
image VARs per run (`input`, `mask`, `cleaned`) and at minimum one
expects to see ledger entries during inflight transfer. Either:
  (a) by the time the driver issues `cmd:image_pool_stats` (between
      runs), every per-run handle has already been released — i.e.
      the snapshot is taken at a quiescent point and looks empty by
      design, OR
  (b) the script-side `VAR(image, ...)` images aren't tracked in
      this ledger at all.

Either way the doc would benefit from a sentence like "stats reflect
*currently-held* handles; per-run images are released by the time
the next `cmd:run` returns". Right now I can't tell from the empty
result whether things are working great or whether the ledger just
doesn't see what I'd expect.

To get a meaningful number I'd want a "since reset" cumulative
counter, or a snapshot triggered *during* a run.

### Anywhere else the framework / SDK / docs made you guess?

- `cmd:open_project` arg key is `folder` per the existing
  `circle_counting/driver_v2.py`; the SDK exposes `open_project(path)`
  but the prose doesn't say whether `path` should be the folder or
  the `project.json` file. I copied the `folder=` form from the
  prior driver. Worth nailing down in `docs/protocol.md`.
- `xi::ops::close` (used here) is fine, but I had to grep
  `xi_ops.hpp` to confirm the signature `close(Image, int radius)`
  rather than rely on docs. A short ops cheatsheet at
  `docs/reference/ops.md` would save the grep.
- `xi::Param` — task said the project should be "user-tunable". I
  considered adding a couple of `xi::Param<int>` to `inspect.cpp`
  but the tunables already live on the two plugins (with sliders),
  so the script's params block is empty. The `params: []` in
  `project.json` works but it's not obvious from existing examples
  whether that's a smell or fine. I went with "fine" because the
  plugin UIs are the user surface here.
- Plugin-cell area thresholds: I picked them deterministically from
  the published bucket radii in `ground_truth.json` (`r²·π`) and put
  them at the midpoints of the gaps. This worked first try; **all
  15 frames perfect, all error totals 0**. For a real defect-detect
  task I'd expect at least one tuning round through the UI sliders.

## Bottom line

- Success criteria: target was ≥12/15 perfect and bucket error ≤5.
  Got 15/15 perfect and bucket error 0 across all three buckets,
  no slider tuning needed. The wide gaps between bucket area ranges
  (max small ≈ 201, min medium ≈ 380; max medium ≈ 615, min large
  ≈ 1018) make this an easy target if the mask is clean — and the
  reused `local_contrast_detector` produces a clean mask.
- Tools shakedown: manifest works (visible end-to-end), imread+
  current_frame_path is a real win, `recent_errors` and
  `image_pool_stats` returned empty (probably correct, but the
  signals are weak — a happy-path run can't differentiate "working"
  from "no-op").

