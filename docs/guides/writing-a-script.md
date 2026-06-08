# Writing an inspection script

The inspection **script** is the C++ file the user authors per project
that runs once per inspection cycle. It's compiled to a DLL by the
backend's `cl.exe` driver and hot-reloaded on save.

This guide walks through the surface area an inspection author actually
uses, with pointers into the deeper reference for each piece.

> **Editor IntelliSense.** Opening a project in the VS Code extension writes a
> `.vscode/c_cpp_properties.json` into the project folder that matches the
> compiler's real include set (the extension reads the resolved paths from the
> backend via `cmd:toolchain_health` — the backend core no longer touches
> `.vscode`). With the
> Microsoft C/C++ extension installed, `<xi/...>` and OpenCV headers resolve,
> `VAR`/`EMIT`/`XI_SCRIPT_EXPORT` are known (the script support header is
> force-included), and go-to-definition works with no false errors. The file is
> auto-generated — to hand-tune it, delete its `"_generated_by": "xinsp2"` stamp
> and the backend will stop overwriting it. See
> [`protocol.md`](../protocol.md) → `open_project`.

---

## The shape

```cpp
#include <xi/xi.hpp>           // xi::Image, xi::Param, VAR, xi::Record, OpenCV
#include <xi/xi_use.hpp>

// File-scope: parameters tunable from the UI without recompile.
xi::Param<int>    thresh{ "threshold", 128, {0, 255} };
xi::Param<double> sigma { "sigma",     2.0, {0.1, 10.0} };

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& cam = xi::use("cam0");        // backend-managed instance
    auto& det = xi::use("detector0");

    auto img = cam.grab(500);
    if (img.empty()) return;

    VAR(input, img);                                 // visible in viewer

    // Image ops: call cv:: directly. xi::Image::as_cv_mat() returns a
    // non-owning view over the same bytes; for outputs that you want
    // the next plugin / VAR to consume zero-copy, use a fresh
    // pool-backed Image and write into its as_cv_mat().
    cv::Mat src = img.as_cv_mat();
    cv::Mat gray_mat;
    cv::cvtColor(src, gray_mat, cv::COLOR_RGB2GRAY);

    cv::Mat blur_mat;
    int k = (int)(sigma * 2 + 1) | 1;        // odd-sized kernel
    cv::GaussianBlur(gray_mat, blur_mat, cv::Size(k, k), (double)sigma);

    // Wrap a cv::Mat back as xi::Image so it crosses the plugin ABI.
    // For inspection scripts this is a one-shot copy; project plugins
    // skip it by using xi::Image::create_in_pool(host(), ...) + cv::
    // writing directly into pool memory (see docs/guides/adding-a-plugin.md).
    xi::Image blur(blur_mat.cols, blur_mat.rows, 1, blur_mat.data);

    auto result = det.process(xi::Record()
        .image("gray", blur)
        .set("threshold", (int)thresh));             // slider value, no recompile

    VAR(detection, result);
    VAR(pass, result["blob_count"].as_int() <= 3);
}
```

That's a full script. Three constructs do the heavy lifting:

| Primitive | Purpose | Lifetime |
|---|---|---|
| `xi::Instance<T>` / `xi::use("name")` | Persistent stateful object (camera, template, model) | Lives across hot-reloads, persisted by host |
| `xi::Param<T>` | Tunable scalar with UI slider | Per script DLL, restored from `project.json` on reload |
| `VAR(name, expr)` | Tracked variable sent to viewer panel | Per `inspect_entry` invocation |

Plus:
- `xi::Record` — the universal data container (named images + JSON).
- `xi::async(fn, args...)` — parallel ops (returns `Future<R>`).
- `xi::breakpoint("label")` — pause script until UI clicks resume.
- `xi::state()` — persistent JSON dictionary that survives hot-reloads.

---

## Lifecycle

```
              ┌──────── on save (auto) ─────────┐
              │                                 ▼
[user edits]→[host compiles via cl.exe]→[load DLL]
              │                                 │
              │                  on cmd:run     ▼
              │              ┌── xi_inspect_entry(frame) ──┐
              │              │  │                          │
              │              │  ├ xi::use(...)             │
              │              │  │   .process(...)          │
              │              │  ├ xi::Param<T> reads       │
              │              │  ├ VAR(name, value)         │
              │              │  └ xi::breakpoint(...)?     │
              │              │       (host pauses worker)   │
              │              └────────────────────────────┘
              │
              └─ on next save: ─→ get_state() (JSON) ─→ unload DLL ─→
                 load new DLL ─→ set_state(JSON) ─→ restore params ─→ ready
```

State that survives the reload:
- `xi::state()` JSON (persisted by `xi_script_get_state` /
  `xi_script_set_state`).
- `xi::Param<T>` values (replayed by `xi_script_set_param`).
- Subscription / history snapshots (host-managed, scoped to the
  client).

State that does NOT survive:
- Static / global C++ objects in your script (the DLL is unloaded).
- Anything you stored in plain heap.

If you need persistence, write to `xi::state()`.

---

## `xi::use<T>` — calling plugins

The host owns instances; the script proxies to them.

```cpp
auto& det = xi::use("detector0");
auto out  = det.process(xi::Record().image("gray", img).set("t", 50));
```

For typed plugin classes (when the plugin is in the same source tree
or you've imported a typed proxy):

```cpp
auto& det = xi::use<MyDetector>("detector0");
det->set_def(R"({"threshold": 50})");
```

Image sources implement `xi::ImageSource` (subclass of `xi::Plugin`)
and add `grab(timeout_ms)` / `grab_wait(timeout_ms)` for pulling frames
synchronously.

`xi::use` works seamlessly across script reloads: the proxy object
re-resolves to the host's current instance after each load.

---

## `xi::Param<T>` — tunable parameters

Declared at file scope. Host syncs them to the UI panel and persists in
`project.json`.

```cpp
xi::Param<int>    thresh { "threshold", 128, { 0, 255 }   };
xi::Param<double> sigma  { "sigma",     2.0, { 0.1, 10.0 } };
xi::Param<bool>   invert { "invert",    false             };

void xi_inspect_entry(int frame) {
    int t = thresh;            // implicit cast to T
    if (invert) {/*…*/}
}
```

When the user drags a slider, `cmd:set_param` updates the value; the
next `cmd:run` picks up the new value. No compile.

For richer config (nested objects, arrays), use `xi::state()` instead
or expose it through a plugin's `set_def`.

---

## `VAR(name, expr)` — variable inspection

```cpp
VAR(gray, toGray(img));            // xi::Image
VAR(t,    thresh);                 // int
VAR(pass, blob_count <= 3);        // bool
VAR(blobs, result["blobs"]);       // xi::Record sub-tree (auto-rendered)
```

Every `VAR(...)` ships a snapshot to the viewer panel after
`inspect_entry` returns. Renderers exist for number, bool, string,
image, and Record (recursive tree).

`VAR(string_literal, ...)` — backed by `std::string`. There's no
lifetime bug: the macro copies into a `std::string` value.

To surface an intermediate **`cv::Mat`** (a mask, a response image), wrap it with
**`xi::from_cv_mat(m)`** — it copies into an owning `xi::Image` so there's no
lifetime trap: `VAR(mask, xi::from_cv_mat(mask_mat));`. See
[`../reference/image-io.md`](../reference/image-io.md) for `xi::Image` / `imread`
/ `as_cv_mat` / `from_cv_mat` and the RGB-not-BGR gotcha.

> **`VAR(name, ...)` declares a local; use `EMIT(name)` to surface an
> existing one.** `VAR` expands to roughly `auto name = expr; <ship to
> viewer>`, so `name` becomes a real variable in the enclosing scope —
> you **cannot** `VAR(count, count)` to surface a value you already
> computed (it redefines `count`; cl.exe fires C2374). For that, use
> **`EMIT(name)`**, which ships an existing in-scope variable without
> declaring anything:
>
> ```cpp
> int count = blobs.size();
> EMIT(count);          // surfaces `count` — no redeclaration
> EMIT(frame);          // works on parameters too
> ```
>
> Rule of thumb: `VAR` to **declare and surface** in one line; `EMIT` to
> **surface something you already have**. (`EMIT_RAW` skips JPEG preview,
> like `VAR_RAW`.)

## `xi::result(code, msg)` — the one per-run verdict

`VAR` ships *many* per-run inspection values (debug detail). `xi::result` ships the
**single verdict record** for the run — the thing MES / PLC / the HMI verdict-yield
cards consume. Exactly one Result per run (last write wins); a run that calls no
`xi::result` defaults to `0` (NA).

```cpp
#include <xi/xi_result.hpp>

void xi_inspect_entry(int frame) {
    if (chip > 0.3)      xi::ng(2, "edge chip > 0.3mm");   // code -2  (ng2)
    else if (smudge)     xi::ng(1, "surface smudge");      // code -1  (ng1)
    else                 xi::ok(1, "clean");               // code +1  (ok1)
}
```

Code convention: **sign = verdict, magnitude = sub-class** — `>0` ok-class,
`0` NA/none, `-1…` ng-class. The band `<= -990000` is **reserved** for framework
system-fails (a dropped frame is auto-emitted as `XI_SYS_DROPPED`); if a script
passes a code in that band the host records the run as NA (`0`) and **logs a
warning naming the bad code** (it doesn't silently fake a verdict). The host emits
a `run_result` event per run (and one
per *dropped* trigger, so the stream has no gaps). Full spec + the system enum:
[`../design/run-result.md`](../design/run-result.md).

## Reading a frame from disk

The host can hand the script a `frame_path` per `cmd:run` (the Python
SDK's `c.run(frame_path=...)`); the script reads it via
`xi::current_frame_path()` and decodes via `xi::imread()`:

```cpp
#include <xi/xi.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    VAR(frame_path, xi::current_frame_path());
    VAR(input,      xi::imread(xi::current_frame_path()));
    if (input.empty()) {
        VAR(error, std::string("frame load failed"));
        return;
    }
    // ... pipeline
}
```

`imread` returns an empty `xi::Image` on failure (file missing,
unsupported format). It accepts PNG / JPEG / BMP / TGA / GIF / PSD /
HDR / PIC via the host's bundled stb_image. Pixels are copied into
the script's own `xi::Image` so the image lifetime is decoupled
from the host pool.

If the run was started with no `frame_path` arg, `current_frame_path()`
returns an empty string. Scripts that always need a path should error
out explicitly when they see one.

For images, the panel shows a thumbnail; double-click (or shift-click)
opens the **interactive image viewer** with pan + cursor-anchored zoom +
Pick Point / Pick Area tools.

---

## `xi::Record` — the universal container

Named images + JSON metadata. Used as the input + output of
`Plugin::process`, returned by ops, stored as VARs.

```cpp
xi::Record r;
r.set("count", 5)                       // chains
 .set("pass",  true)
 .image("binary", img);                 // attach an image

int n  = r["count"].as_int(0);          // safe defaults
bool p = r["pass"].as_bool(false);
auto& im = r.get_image("binary");

// Path access (works for nested objects + arrays)
int x  = r["roi.x"].as_int();
auto v = r["points[0].score"].as_double();
```

cJSON-backed; copies are cheap until you mutate (copy-on-write
behaviour for the JSON tree). Images are refcounted via the host pool —
no copy when passing through `process()`.

---

## `xi::async` — parallel ops

```cpp
auto fa = xi::async([&]{ cv::Mat g; cv::cvtColor(src, g, cv::COLOR_RGB2GRAY); return g; });
auto fb = xi::async([&]{ cv::Mat b; cv::GaussianBlur(src, b, {0,0}, 2.0); return b; });
auto [g, b] = xi::await_all(fa, fb);    // tuple<cv::Mat, cv::Mat>
```

Wrap the cv:: call in a lambda so `xi::async` can capture by value /
reference as needed. `Future<T>` is consumed once; reuse compiles to an
error.
SEH-translated, so a segfault inside a parallel branch surfaces as an
exception at the await site rather than crashing the backend.

---

## `xi::state()` — persistent JSON

```cpp
auto& s = xi::state();
s.set("calibrated", true);
s.set("offset_x",   12.5);
auto count = s["counter"].as_int(0);
s.set("counter", count + 1);
```

Survives:
- Hot-reload (DLL unload + reload).
- Backend restart (host writes to disk on shutdown).
- Project re-open.

Use for cross-frame counters, calibration results, "have we seen this
serial number" caches.

---

## `xi::breakpoint(label)` — pause for UI

```cpp
VAR(thresh_img, threshold(gray, t));
xi::breakpoint("after-threshold");      // worker parks here
```

When the host's continuous-run mode hits this line, it sends an event
to the UI and parks the worker. The user clicks **Resume** in the
status bar (or runs `cmd:resume`) to release. Useful for tuning at a
specific stage.

---

## Triggers / multi-camera

For multi-camera setups, sources publish frames under a 128-bit
trigger ID; the host's TriggerBus correlates and dispatches one
inspect call per complete trigger.

Inside the script:

```cpp
auto t = xi::current_trigger();
if (!t.is_active()) return;

auto left  = t.image("cam_left");
auto right = t.image("cam_right");
VAR(disp, stereo_match(left, right));
```

The `is_active()` guard is **required** in continuous mode
(`cmd:start fps=N`). The host runs two dispatch sources side-by-side:
the trigger bus dispatches one inspect call per complete trigger AND a
wall-clock timer dispatches one per frame regardless of whether a
trigger fired. The timer-driven dispatches arrive with no trigger
attached (`is_active() == false`); without the guard your script
would null-deref / read empty Images on those ticks. In single-shot
mode (`cmd:run`) this distinction doesn't apply — there's exactly one
dispatch per command.

See [`docs/architecture.md`](../architecture.md) for bus policies (Any
/ AllRequired / LeaderFollowers) and the `synced_stereo` reference
plugin.

### `xi::Trigger` accessors

| Accessor                  | Returns                                                               |
| ------------------------- | --------------------------------------------------------------------- |
| `t.is_active()`           | `false` for synthetic timer ticks; `true` once a real event landed    |
| `t.id()` / `t.id_string()`| 128-bit trigger id (struct or 32-char hex)                            |
| `t.timestamp_us()`        | μs since Unix epoch when the source called `host->emit_trigger`       |
| `t.dequeued_at_us()`      | μs (same clock) when the dispatcher worker popped this event          |
| `t.image(name)`           | the named source's frame, zero-copy view                              |
| `t.sources()`             | list of source names present in this event                            |
| `t.primary_source()`      | leader source name (policy-aware); falls back to `sources().front()`  |
| `t.has_source(name)`      | `true` if `name` appears in `sources()`; routing without manual hash  |

#### Routing by source identity

Multi-source scripts often need different processing per source. Before
P2-2 the idiom was to stamp an FNV-1a hash into pixel bytes [8..15] at
the plugin and recompute it at the script — workable but awkward. The
direct path is now:

```cpp
auto t = xi::current_trigger();
if (!t.is_active()) return;

// Single-source case (policy=any with one source): primary == only source
if (t.has_source("camera_left")) {
    auto img = t.image("camera_left");
    // ...
}

// Leader/follower case (policy:"leader_followers"): primary is the leader
if (t.primary_source() == "camera_top") {
    // run the leader-specific pipeline
}
```

`primary_source()` is policy-aware on the host side: for `policy:"any"`
it's whichever instance emitted; for `leader_followers` it's the
configured leader; for `all_required` it may be empty (consult
`sources()` instead).

### Latency: queue-wait vs inspect-time

End-to-end latency (`now - emit_ts`) lumps two very different things
together: time the trigger sat in the dispatch queue waiting for a free
worker, and time spent actually executing `xi_inspect_entry`. Under
bursty load with `dispatch_threads > 1` the distribution goes bimodal —
median is the post-surge tail running at full pipeline; p95 is the
front of the surge waiting in queue. You can't tell which one is your
bottleneck from the lump.

Use `dequeued_at_us()` to split:

```cpp
auto t = xi::current_trigger();
if (!t.is_active()) return;

int64_t now = xi::now_us();
double queue_wait_us = (double)(t.dequeued_at_us() - t.timestamp_us());
double inspect_us    = (double)(now              - t.dequeued_at_us());

VAR(queue_wait_us, queue_wait_us);   // queue saturated → grows during surge
VAR(inspect_us,    inspect_us);      // your code's actual cost
```

Both clocks are `std::chrono::system_clock` microseconds, so subtraction
is meaningful across the host/script boundary. Reading from
`examples/multi_source_surge/`, a 200-frame surge into `queue_depth=32`
with a single ~30 ms inspect produces `queue_wait_us` that grows roughly
linearly across the surge (FIFO accumulation) while `inspect_us` stays
near 30 ms — which is the diagnostic signature of "I need more
`dispatch_threads`," not "my inspect is too slow." Bumping to N=8 with
`queue_depth=128` collapses `queue_wait_us` while leaving `inspect_us`
unchanged.

`dequeued_at_us()` is 0 in single-shot `cmd:run` and on synthetic timer
ticks where there's no trigger event — always check `is_active()`
first, and treat 0 as "no split available, fall back to end-to-end
latency."

## Parallel dispatch (`parallelism.dispatch_threads`)

By default `cmd:start` runs one dispatcher thread — every inspect call
is serial. Add to `project.json`:

```json
{
  "name": "my_project",
  "script": "inspect.cpp",
  "parallelism": {
    "dispatch_threads": 4,
    "queue_depth": 100,
    "overflow": "drop_oldest",
    "result_order": "completion"
  }
}
```

…to fan out across **N concurrent inspect calls**. A burst of 4 frames
arriving in the same 10 ms window now lands on 4 worker threads that
all run `xi_inspect_entry` simultaneously. See `examples/burst_dispatch/`
for a baseline measurement; with `sleep_ms=50` per inspect and
`fps=100`, N=1 yields ~16 events/sec, N=4 yields ~58.

`queue_depth` (default 100) bounds how many trigger events buffer
when workers are busy. `overflow` picks the policy when the queue
fills:

- **`drop_oldest`** (default): pop front, push new. Latest frame
  always gets in. Right for live inspection where stale frames
  are useless.
- **`drop_newest`**: refuse new, preserve FIFO. Right when downstream
  ordering matters (archival, ML training capture).
- **`block`**: `emit_trigger` blocks until room. Back-pressure to
  the source. Right when the source itself can throttle.

`result_order` controls how per-frame results land on the wire under N > 1:

- **`completion`** (default): emit as each worker finishes — lowest latency,
  but with uneven inspect times the stream is out of frame order (sort
  client-side by `run_id` if you care).
- **`arrival`**: emit in frame-arrival order. A worker that finishes early
  waits its turn before emitting, so the `vars`/preview/`run_finished` stream
  matches trigger order and `run_id` is monotonic on the wire. Compute still
  runs fully parallel; only emission is gated (a small latency cost). Use it
  when a downstream consumer assumes in-order results. See
  `examples/qa_result_order/`.

Probe live state with `cmd:dispatch_stats` (Python: `c.call("dispatch_stats")`):

```python
{ "queue_depth_now": 4,
  "queue_depth_cap": 100,
  "queue_depth_high_watermark": 27,
  "overflow": "drop_oldest", "dispatch_threads": 4,
  "dropped_oldest": 88, "dropped_newest": 0 }
```

- `queue_depth_now` — current size.
- `queue_depth_cap` — the configured `queue_depth` from project.json.
- `queue_depth_high_watermark` — peak depth observed since the last
  `cmd:start`. Counters reset on each `cmd:start`. **This is the
  real tuning signal**: if peak << cap you have headroom; if peak
  == cap you're saturating.
- `dropped_oldest` / `dropped_newest` — overflow counters since
  last `cmd:start`.

> **Don't subtract a pre-`cmd:start` snapshot from a post-`cmd:stop`
> one.** The counters and the high-watermark zero on every `cmd:start`,
> so `after - before` will go negative across run boundaries. Snapshot
> AFTER stop and treat the values as the per-run total.

**Priority lanes (dispatch groups).** For "critical work must stay fast,
best-effort may lag", give `parallelism.groups` instead of a single pool: each
group owns its own `max_parallel` worker threads at its OS `thread_priority`, and
a source's triggers route to the group named in its `instance.json` `"group"`. A
saturated low-priority group can't steal the critical group's threads or CPU.
`dispatch_stats` then includes a per-group breakdown. See
[`../design/dispatch-groups.md`](../design/dispatch-groups.md).

If `queue_depth_high_watermark` stays pinned at the cap and
`dropped_oldest` keeps growing, your source is producing faster than
your pipeline can keep up — bump `dispatch_threads`, optimise the
plugin, or accept the drops.

**Plugin instances are safe by default — declared reentrancy.** A plugin
called via `xi::use("det").process(...)` from N dispatch workers would
otherwise see N concurrent `process()` calls into the *same* instance. To
keep `dispatch_threads > 1` safe out of the box, the host **serializes calls
per instance with a mutex** (covering `process` / `exchange` / `get_def` /
`set_def`, so a live config change can't race an in-flight frame). A plugin
opts into true per-instance parallelism by declaring, in its `plugin.json`:

```json
{ "name": "det", "dll": "det.dll", "reentrant": true }
```

`reentrant: true` (alias: `thread_safe: true`) tells the host "my `process()`
is safe to call concurrently on one instance" — then it runs N-up with no
lock. Leave it off and your plugin can never be re-entered, whatever
`dispatch_threads` is. Parallelism still flows across *different* instances
either way; the lock only serializes a single non-reentrant instance.

**Per-instance concurrency cap.** A reentrant plugin can be bounded to *M*
concurrent calls on a given instance via `max_concurrency` in that instance's
`instance.json` (e.g. a reentrant detector backed by 2 GPU streams):

```json
{ "plugin": "det", "max_concurrency": 2 }
```

The host then admits at most M workers into that instance's entry points at once
(a counting semaphore; the non-reentrant lock is just the M=1 case). `0`/absent =
unlimited (full `dispatch_threads`-wide). Ignored for a non-reentrant plugin
(always 1). Lets one slow/resource-bound instance run narrower than the pool
without throttling the rest. `examples/qa_reentrancy/` proves all three:
serialized (non-reentrant → 1), concurrent (reentrant → N), and capped
(reentrant + `max_concurrency: 1` → 1).

**Other caveats once N > 1 (your responsibility):**

- **`xi::state()`** is a single shared dict. Concurrent reads/writes
  race. Wrap mutations in your own `std::mutex`, or design the
  pipeline so only one thread writes a given key.
- **Reentrant plugins** (those that opted in above) must themselves be
  thread-safe: cv:: ops on pool-backed Images are mostly fine; member
  counters / caches are not — guard them with atomics or a mutex.
- **Watchdog now covers every worker** (it tracks a deadline slot per
  in-flight inspect, not a single slot). On a deadline breach it asks the
  script to cancel cooperatively — but that flag is **global**, so under
  N > 1 it aborts *every* in-flight frame that round (healthy workers just
  re-run next tick). If the script ignores cooperative cancel, the backend
  **exits** so the FE supervisor respawns a clean one — it does **not**
  force-kill a worker (that would leak the per-instance lock). Long ops
  should poll `xi::cancellation_requested()` so a cooperative cancel takes.
- **`vars` events** arrive interleaved across run_ids in the default
  `result_order: "completion"`. Set `result_order: "arrival"` (above) for an
  in-order wire stream, or sort client-side by `run_id`.
- **`xi::Param<T>`** reads are atomic and safe.
- **VAR** writes go to a thread-local ValueStore — each dispatcher
  has its own.

When in doubt, leave `dispatch_threads` at 1.

---

## Using an external library / DLL

Two ways to pull a third-party SDK into a script:

**Best for anything reusable — wrap it in a plugin.** Plugins can ship their
dependency DLLs in their own folder and get a config UI; the script just
`xi::use()`s them. See [`adding-a-plugin.md`](./adding-a-plugin.md).

**Directly in the script — declare it in `project.json`:**

```jsonc
{
  "name": "my_project",
  "script": "inspect.cpp",
  "include_dirs": ["include", "C:/abs/sdk/include"],  // extra cl /I (relative = from project)
  "link_libs":    ["deps/foo.lib"]                     // import libs to link (relative = from project)
}
```

Then `#include <foo.h>` and call into it. At runtime the dependency DLL
(`foo.dll`) must be found: the backend puts the **project folder** on the DLL
search path, so dropping `foo.dll` in the project folder works (it's also fine
next to `xinsp-backend.exe`). Relative `include_dirs`/`link_libs` resolve against
the project folder. Worked end-to-end example:
[`examples/script_external_dll`](../../examples/script_external_dll).

> Why the project folder and not next to the script: the compiled script DLL
> lives in a temp build dir, loaded with `LOAD_LIBRARY_SEARCH_USER_DIRS` so only
> the app dir + System32 + the project folder are searched (CWD/PATH are not).

If you'd rather not touch `project.json`, you can also `LoadLibraryEx` the DLL
yourself by absolute path inside the script and `GetProcAddress` — that keys on
the full path and sidesteps search rules entirely.

---

## Common pitfalls

- **Forgetting `XI_SCRIPT_EXPORT`** on `xi_inspect_entry`. The host's
  loader will report "missing entry point" and refuse to load.
- **Holding raw pointers across reloads**. The DLL's static memory is
  gone after `unload_script`. Use `xi::state()` for persistence.
- **Stack overflow / heap corruption** still crashes the process —
  SEH translation handles segfaults / div0 / array overrun, not
  unbounded recursion or write-past-buffer-end. The script runs
  in-process; on a hard crash the backend auto-respawns and writes a
  crash report + minidump (see [`debugging.md`](./debugging.md)).
- **`xi::Param<T>` declared inside `xi_inspect_entry`** — won't be
  registered. They must be at file scope.

---

## Where to look next

- [`docs/reference/host_api.md`](../reference/host_api.md) — the C ABI
  the script's plugins consume.
- [`docs/protocol.md`](../protocol.md) — the WS commands a UI client
  sends to drive a script (`run` / `set_param` / `compile_and_load` /
  …).
- [`examples/`](../../examples/) — working scripts:
  `defect_detection.cpp`, `use_demo.cpp`, `user_with_instance.cpp`.
