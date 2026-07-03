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
> [`protocol.md`](../reference/ws-protocol.md) → `open_project`.

---

## The shape

```cpp
#include <xi/xi.hpp>           // xi::Image, xi::Param, xi::Record (OpenCV-free umbrella)
#include <xi/xi_cv.hpp>        // OpenCV interop — needed because we call cv:: below
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>    // xi::ok / xi::ng — the per-run verdict

// File-scope: parameters tunable from the UI without recompile.
xi::Param<int>    thresh{ "threshold", 128, {0, 255} };
xi::Param<double> sigma { "sigma",     2.0, {0.1, 10.0} };

// Explicit-trigger entry (preferred). The host passes the trigger in as `t`,
// so it's self-contained — no ambient thread_local state. `frame` is the id.
XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;         // skip synthetic timer ticks
    auto img = t.image("frame");        // the frame a source emitted
    if (img.empty()) return;

    // Image ops: call cv:: directly. The free `xi::as_cv_mat(img)` (from
    // <xi/xi_cv.hpp>) returns a non-owning view over the same bytes; for
    // outputs you want the next plugin to consume zero-copy, use a fresh
    // pool-backed Image and write into `xi::as_cv_mat()` of it.
    cv::Mat src = xi::as_cv_mat(img);
    cv::Mat gray_mat;
    cv::cvtColor(src, gray_mat, cv::COLOR_RGB2GRAY);

    cv::Mat blur_mat;
    int k = (int)(sigma * 2 + 1) | 1;        // odd-sized kernel
    cv::GaussianBlur(gray_mat, blur_mat, cv::Size(k, k), (double)sigma);

    // Copy a cv::Mat back into an OWNING xi::Image so it crosses the plugin
    // ABI. For inspection scripts this one-shot copy is fine; project plugins
    // skip it by writing cv:: output straight into pool memory (see
    // docs/guides/write-a-plugin.md).
    xi::Image blur = xi::from_cv_mat(blur_mat);

    auto result = xi::use("detector0").process(xi::Record()
        .image("gray", blur)
        .set("threshold", (int)thresh));             // slider value, no recompile

    int blob_count = result["blob_count"].as_int();

    // Surface per-run values/images to a UI via the `expose` sink (this
    // replaces the old VAR path). Requires an `expose` instance in project.json.
    xi::use("expose").process(xi::Record()
        .set("$channel", "inspection")
        .image("input", img)
        .set("blob_count", blob_count));

    if (blob_count <= 3) xi::ok(1, "clean");          // the run's verdict
    else                 xi::ng(1, "too many blobs");
}
```

That's a full script. Three constructs do the heavy lifting:

| Primitive | Purpose | Lifetime |
|---|---|---|
| `xi::use("name")` → `xi::UseProxy&` | Proxy to a backend-managed instance (camera, model, etc.) | Instance lives across hot-reloads, persisted by host |
| `xi::Param<T>` | Tunable scalar with UI slider | Per script DLL, restored from `project.json` on reload |
| `xi::use("expose").process(rec)` | Surface per-run values/images to a UI (replaces VAR) — see [below](#surfacing-output--the-expose-plugin) | Per `inspect_entry` invocation |
| `VAR(name, expr)` | **Legacy no-op.** Expands to `auto name = expr;` and publishes nothing — see [appendix](#appendix-legacy-varemit-compatibility) | Per `inspect_entry` invocation |

> **VAR is legacy — surface output through the `expose` plugin.** The core's VAR
> value-tracking, the `vars` wire message, and the old JPEG image-preview path were
> **removed**. `VAR(...)` / `EMIT(...)` still **compile** (so existing scripts build
> unchanged) but publish nothing. To show per-run values/images in a UI, build a
> plain `xi::Record` and push it to the **`expose` plugin** with
> `xi::use("expose").process(rec)` — the shipped data-out surface, documented
> [below](#surfacing-output--the-expose-plugin). The run's pass/fail
> verdict still leaves via `xi::result(...)` (also below).

Plus:
- `xi::Record` — the universal data container (named images + JSON).
- `xi::async(fn, args...)` — parallel ops (returns `Future<R>`).
- `xi::state()` — persistent JSON dictionary that survives hot-reloads.

---

## The entry point — `XI_INSPECT_ENTRY` (preferred) vs the legacy signature

There are **two** ways to declare the entry, and new scripts should use the first:

```cpp
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

// PREFERRED (script-entry ABI v2, "A4"): the host hands you the trigger EXPLICITLY.
XI_INSPECT_ENTRY(t, frame) {          // `t` is a xi::Trigger, `frame` is int
    if (!t.is_active()) return;
    auto img = t.image("frame");
    // ... inspect ...
}
```

```cpp
// LEGACY (still supported forever): the trigger is ambient thread-local.
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto t = xi::current_trigger();   // reads the ambient thread_local
    if (!t.is_active()) return;
    // ... inspect ...
}
```

`XI_INSPECT_ENTRY(t, frame)` expands to the exported entry
`xi_inspect_entry_tv(const xi_trigger_view*, int)` and builds a **self-contained**
`xi::Trigger t` from a host-filled view — image handles + meta doc + the host API
to resolve them all travel *in the argument*, with **no ambient `thread_local`
seam**. Two concrete wins over `xi::current_trigger()`:

- **`t` is valid on ANY thread** and is **safe to capture by value** into
  `xi::async` / `xi::parallel_for` — it is exactly the `trigger_snapshot()` you
  otherwise had to make by hand (see [Parallelism safety](#parallelism-safety--three-rules-for-worker-threads),
  rule 1). No accidental cross-thread read of the ambient trigger.
- The `t.*` accessor table below is **identical** for both entries — only *how you
  obtain `t`* differs. Everything else in this guide (`xi::use`, `xi::Param`,
  `expose`, `xi::result`, `xi::state`) is unchanged.

A script exports **exactly one** of the two — the loader resolves
`xi_inspect_entry_tv` in preference to `xi_inspect_entry`, and falls back to the
legacy symbol when the new one is absent, so old scripts run untouched. Everywhere
below that shows `void xi_inspect_entry(int)` + `xi::current_trigger()` is the
legacy form; the `XI_INSPECT_ENTRY(t, frame) { … }` form is a drop-in swap where
`t` replaces the `current_trigger()` call.

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
              │              │  └ xi::result(code, msg)    │
              │              └────────────────────────────┘
              │
              └─ on next save: ─→ get_state() (JSON) ─→ unload DLL ─→
                 load new DLL ─→ set_state(JSON) ─→ restore params ─→ ready
```

State that survives the reload:
- `xi::state()` JSON (persisted by `xi_script_get_state` /
  `xi_script_set_state`).
- `xi::kv()` canonical-mp bytes (persisted by `xi_script_kv_get` /
  `xi_script_kv_set` — the post-Record channel, see *`xi::kv()`* below).
- `xi::Param<T>` values (replayed by `xi_script_set_param`).

State that does NOT survive:
- Static / global C++ objects in your script (the DLL is unloaded).
- Anything you stored in plain heap.

If you need persistence, write to `xi::state()`.

---

## `xi::use` — calling plugins

The host owns instances; the script proxies to them via `xi::UseProxy`.

```cpp
auto& det = xi::use("detector0");
auto out  = det.process(xi::Record().image("gray", img).set("t", 50));
```

`UseProxy` exposes `process(Record)` and `exchange(string)`.

`xi::use` works seamlessly across script reloads: the proxy
re-resolves to the host's current instance after each load.

> **`xi::use("name")` vs `xi::Instance<T>` — two distinct tools, not two ways to
> do one thing.** `xi::use("name")` reaches a **host-registered** instance (created
> via the UI / `cmd:create_instance`, owned and persisted by the host) — this is
> the normal path and comes in through the `xi/xi.hpp` umbrella. `xi::Instance<T>`
> is a **script-owned** instance you declare and own *inline* in the script — an
> advanced capability you opt into explicitly with `#include <xi/xi_instance.hpp>`
> (it is no longer auto-included by the umbrella). Reach for `xi::use` first; use
> `xi::Instance<T>` only when you deliberately want the script to own the instance.

Image sources are ordinary plugins too — they don't sit behind a pull/`grab`
API. A source pushes frames by calling `host->emit_record(...)`, and the script
reads the resulting frame from `xi::current_trigger()` (see *Triggers /
multi-camera* below), not by polling the source proxy.

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

## Output channels at a glance — which one do I use?

Historically a script had six-plus ways to emit something (VAR, EMIT, result,
status, expose, emit_binary). That is now **two blessed surfaces** plus one
operator-status side-channel. When you ask *"how do I show this value?"* the
answer is almost always **expose**:

| I want to… | Use | What it is |
|---|---|---|
| Ship the run's **pass/fail verdict** (one per run) | `xi::result(code,msg)` / `xi::ok(...)` / `xi::ng(...)` | The single verdict record MES/PLC/HMI consume. Exactly one per run, last-write-wins. |
| Show **any other value or image** (scores, counts, previews, debug channels) | `xi::use("expose").process(rec)` | The official observational data-out surface. Build a plain `xi::Record`, tag `"$channel"`, push. |
| Set the operator-facing **"what am I doing right now"** string | `xi::status("...")` | One sticky, human, last-write-wins status line (`xi_status.hpp`). Not a value, not a log. |
| Write a diagnostic **log line** | `host->log(...)` / plugin `log_*` | Event stream for debugging, not UI values. |

Everything else is legacy: **`VAR(...)` / `EMIT(...)` were removed** (they no longer
compile — `C3861` — see below), and the old
per-key `VAR`/`vars`/JPEG-preview wire paths are gone. There is **no** author-facing
`emit_binary` verb to reach for — image output rides on the `expose` record.

## Surfacing output — the `expose` plugin

VAR used to ship per-run values/images to a viewer panel; that path was removed
from core (see the legacy note below). The shipped replacement is the **`expose`
plugin** — the official script data-out surface: build a plain `xi::Record`, tag it
with a **channel id**, and push it. A UI tabs between channels — per stage / per
thread / per camera. There is **no special header and no macro** — `expose` is
called like any other plugin via the generic `xi::use("expose").process(rec)`:

```cpp
void xi_inspect_entry(int frame) {
    // ... compute img, score, gain ...

    xi::Record r;
    r.set("frame", frame);          // a value
    r.set("score", score);          // a value
    r.image("edges", img);          // an image, tagged by key
    r.set("$channel", "bright");    // channel id (string) — reserved key

    xi::use("expose").process(r);   // generic plugin call
}
```

- **The record *is* the payload.** Build it with the ordinary `xi::Record` API
  (`set(...)` for values, `image(...)` for images). **Display order = the record's
  own key order** — the host preserves insertion order, so fields render
  top-to-bottom in the order you wrote them. No `__LINE__` / `$layout` machinery.
- **`"$channel"`** (reserved key, a string) selects the output channel; it is
  **created implicitly** on first send — no pre-declaration. The host also stamps
  `"$seq"` (= the run id) for ordering. The plugin strips both from the published
  record. Multiple channels per run is fine — set a different `"$channel"` and call
  `process()` again.
- Attaching the **same image buffer** under two keys is cheap: the host
  JPEG-compresses it once (dedup).

> **`expose` is an ordered output sink.** Its `plugin.json` declares
> `"sink": true`, so under parallel dispatch (`parallelism.dispatch_threads > 1`)
> the host **stages** each `use("expose").process(...)` and flushes it in
> **frame-arrival order** (stamping `$seq` = the wire `run_id`) instead of
> worker-completion order. So live output never tears or reorders across workers,
> with no extra work in your script. Any plugin you want frame-ordered the same way
> just sets `"sink": true` — see [`../reference/c-abi.md`](../reference/c-abi.md).

> **Legacy `VAR` / `EMIT`?** They no longer surface anything. See
> [Appendix: legacy `VAR`/`EMIT`](#appendix-legacy-varemit-compatibility) at the
> end of this guide — new scripts should ignore them and use `expose` above.

## `xi::result(code, msg)` — the one per-run verdict

`xi::result` ships the **single verdict record** for the run — the thing MES /
PLC / the HMI verdict-yield cards consume. It is the one script-output path that
remains live in core (VAR/EMIT no longer surface anything — see above). Exactly
one Result per run (last write wins); a run that calls no `xi::result` defaults
to `0` (NA).

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
[`../roadmap/run-result.md`](../roadmap/run-result.md).

## Reading a frame from disk

The host can hand the script a `frame_path` per `cmd:run` (the Python
SDK's `c.run(frame_path=...)`); the script reads it via
`xi::current_frame_path()` and decodes via `xi::imread()`:

```cpp
#include <xi/xi.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    auto input = xi::imread(xi::current_frame_path());
    if (input.empty()) {
        xi::ng(1, "frame load failed");
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

(To view a decoded image back in a UI, push it to the `expose` plugin with
`r.image("name", img)` — see [Surfacing output](#surfacing-output--the-expose-plugin).)

---

## `xi::Record` — the universal container

Named images + JSON metadata. Used as the input + output of
`Plugin::process`, returned by ops, passed between stages.

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

yyjson-backed (a mutable DOM tree). Images are refcounted via the host
pool — no copy when passing through `process()`.

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

### Operator parallelism — picking a tool

| You want | Use | Needs a flag? |
|---|---|---|
| Run several independent operators at once | `xi::async` (above) | no |
| Parallelize one operator's inner loop | `cv::parallel_for_(cv::Range(0,h), …)` — uses OpenCV's thread pool | no |
| A parallel pixel/row loop, **fault-safe** | `xi::parallel_for(n, body)` (below) — SEH-safe, cancellable, owner-attributed | yes (`/openmp`) |
| Process a list of ROIs/blobs each | `std::for_each(std::execution::par, …)` (`#include <execution>`) | no |

> **A raw `#pragma omp` in your script is rejected at compile time** — write the
> loop through `xi::parallel_for` / `xi::async` instead (both are SEH-safe and
> owner-attributed). See [OpenMP (opt-in)](#openmp-opt-in) for why, and the
> `allow_raw_omp` escape hatch.

### OpenMP (opt-in)

`project.json`'s `"openmp_max_threads"` is a **single knob** that both enables
OpenMP and caps its threads (the cap is built into the switch precisely because
of the oversubscription risk below):

| value | effect |
|---|---|
| `0` / absent | OFF — no `/openmp` (default; production compiles unchanged) |
| `N` (>0) | ON, capped to **N** threads (auto `omp_set_num_threads(N)` at load) |
| `-1` | ON, uncapped (all cores) |

Enabling OpenMP turns on `/openmp` **for `xi::parallel_for` / `xi::async`** — the
blessed wrappers that carry the crash-isolation translator onto worker threads.
You write the parallel loop through them, not a hand-rolled pragma:

```jsonc
// project.json
{ "openmp_max_threads": 4 }
```
```cpp
auto snap = xi::trigger_snapshot();          // on the inspect thread
xi::parallel_for(h, [&, snap](int y) {       // honours the cap automatically
    /* per-row work */
});
```

The cap is applied for you at DLL load — you don't call `omp_set_num_threads()`.
Links `vcomp140.dll` (in System32; `tools/export_bundle.py` bundles it for AOT).

> **A raw `#pragma omp` written in your script's own source is rejected at
> compile time** with an error pointing you here (`allow_raw_omp` below overrides
> it). Why: a hardware fault inside a raw omp region runs on a worker thread that
> has **no SEH translator**, so it skips crash isolation and **takes down the
> whole backend**, and any pool image it creates is tagged `owner=0` and leaks
> past the per-script sweep. `xi::parallel_for` / `xi::async` install the
> translator **and** the image-pool owner on every worker, so their faults are
> caught and their images are attributed. (Dropping `/openmp` is not the fix —
> the wrappers are header-only and need it to parallelize.)
>
> **Escape hatch:** if you genuinely need a raw pragma (e.g. a per-loop
> `num_threads` override for IO-wait work) and accept the three worker-thread
> rules below, set `"allow_raw_omp": true` in `project.json`. You then own the
> SEH + owner discipline yourself — the DLL-load warmup installs a translator on
> the persistent OpenMP team as a best-effort floor, but nested / dynamic / grown
> teams spawn fresh untranslated threads.

Caveat — **oversubscription**: inspects already run in parallel across dispatch
threads, and `cv::` ops are internally multi-threaded, so stacking OpenMP on top
can exceed core count and *slow down*. That's why the switch is a thread cap, not
a bool: for **CPU-bound** work set it around `cores ÷ dispatch_threads` and
**measure** end-to-end throughput, not just single-op latency (see
[`../internals/dispatch.md`](../internals/dispatch.md)).

OpenMP is a **CPU-bound** fork-join model — sizing a thread pool to cores. For
**IO-wait** operators (PLC / network / disk), don't reach for OpenMP: the threads
would just block. Use **`xi::async`** — one task per concurrent wait, not tied to
core count, and SEH-safe + owner-attributed like `xi::parallel_for`. (A per-loop
`num_threads(32)` override that beats the global cap needs a raw pragma, so it
requires the `allow_raw_omp` opt-out — prefer `xi::async` for IO-wait fan-out.)
Keep the global `openmp_max_threads` sized for the CPU-bound default.

### Parallelism safety — three rules for worker threads

Your `inspect` runs on the **inspect thread**. Three pieces of ambient context
live in *thread-local* state on that thread and **do not cross** into the worker
threads `xi::async` / `xi::parallel_for` (or, under `allow_raw_omp`, a raw
`#pragma omp` region) spawn. `xi::parallel_for` / `xi::async` handle all three for
you; a raw region makes them your problem. Get them wrong and the failure modes
range from silent-wrong-output to a backend crash:

1. **Read the trigger on the inspect thread; parallel regions consume captured
   locals.** `xi::current_trigger()` / `t.image(...)` are valid *only* on the
   inspect thread. Calling them from a worker fails loud (an abort with a named
   message in debug, a logged error in release). Snapshot first and capture by
   value with **`xi::trigger_snapshot()`**:

   ```cpp
   auto snap = xi::trigger_snapshot();                 // inspect thread
   xi::parallel_for(rows, [&, snap](int y){            // any worker thread
       cv::Mat g = snap.image("gray").as_cv_mat();
       process_row(g, y);
   });
   ```

   The snapshot addref's each image, so the pixels stay valid for the whole
   region; its accessors touch no thread-local.

2. **A C++ exception must not cross a `#pragma omp` boundary** — OpenMP requires
   it caught inside the same structured block. A hardware fault (access
   violation, divide-by-zero) is *SEH*, not a C++ exception, and a worker thread
   the OpenMP runtime spawned has no SEH translator → an escaping fault
   **terminates the whole backend**.

3. **Pool images created on a worker are tagged anonymous (`owner=0`)** — still
   thread-safe, but outside the per-owner leak sweep, so a genuinely leaked one
   isn't reclaimed until process exit and shows as "anonymous" in
   `image_pool_stats`.

**`xi::parallel_for(n, body)` handles all three for you** — and a hand-written
`#pragma omp parallel for` in your script is rejected at compile time (use this):

```cpp
#include <xi/xi.hpp>      // xi::parallel_for comes in via the umbrella

auto snap = xi::trigger_snapshot();
xi::parallel_for(h, [&, snap](int y) {
    // runs across the OpenMP pool; faults here are caught and rethrown
    // on the inspect thread, not propagated out of the omp region.
    process_row(y);
});
```

It installs the SEH translator on each worker, polls
`xi::cancellation_requested()` and skips remaining iterations on a watchdog
cancel, catches every fault inside the region and rethrows the first one on the
inspect thread, and re-installs the inspect-thread image-pool owner per worker so
parallel-created images stay attributed. `xi::async` already does the same
(SEH + owner propagation) for independent-branch fan-out. Needs `/openmp`
(`"openmp_max_threads"` set); without it `xi::parallel_for` runs serially with
identical semantics.

> Raw `#pragma omp` in a script is **rejected at compile time** — the compiler
> error routes you to `xi::parallel_for` / `xi::async`. It is not a style
> preference: a hardware fault in a raw region terminates the whole backend and
> its pool images leak (`owner=0`). If you deliberately opt out with
> `"allow_raw_omp": true`, you own rules 1–3: the script-load **pool warmup**
> installs a translator on the persistent OpenMP team so common-case raw regions
> are covered, but nested / dynamic / grown teams spawn fresh untranslated
> threads — route fault-prone loops through `xi::parallel_for`.

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
- Hot-reload (DLL unload + reload) — the host captures the state from the old
  DLL and restores it into the new one, in memory.

Does NOT survive (deliberately — verified against the host, 2026-07-03; the
project boundary reset is pinned by `qa_param_state_isolation`):
- Backend restart (state is never written to disk).
- Project open/close (a fresh project starts from its own defaults).

Use for cross-frame counters, calibration results, "have we seen this
serial number" caches.

> **Heads-up — `xi::state()` is scheduled for deletion at the Record cut**
> (docs/new_gen/16-script-state-shape.md). Its successor `xi::kv()` (next
> section) is live NOW; new scripts should prefer it, and existing scripts
> can port at leisure during the bilingual window.

### Schema versioning + migration across a code change

By default a hot-reload that **changes the shape** of `xi::state()` would let the
old DLL's persisted JSON default-fill the new shape incorrectly. Guard against it
by declaring a **schema version** at file scope — bump it whenever the state shape
changes:

```cpp
#include <xi/xi_state.hpp>
XI_STATE_SCHEMA(2);          // this script's state is shape-version 2
```

On reload the host compares the saved version to the new one; on a **mismatch** it
**drops** the prior state (emits `event:state_dropped`) rather than restore a
mismatched shape. Version `0` (absent) means "unversioned" — legacy blind-restore.

If you want cross-version **continuity** instead of a drop, register a migrator —
the same discipline as Erlang's `code_change/3`. It runs in the NEW DLL (which
alone knows both shapes), gets the old JSON + versions, and returns the re-shaped
JSON:

```cpp
#include <xi/xi_state.hpp>
static int _mig = (xi::set_state_migrate(
    [](const std::string& old_json, int from, int to) -> std::string {
        // parse old_json, translate shape `from` -> `to`, return new-shape JSON
        return migrated_json;    // return "" to DECLINE -> host drops as usual
    }), 0);
```

Register it at static-init time (it runs at DLL load, before the host's first
`set_state`). The migrator must be **pure** w.r.t. the call — it must not read
live `xi::state()`, which has not been restored yet. Absent registration (or a
`""` return) leaves the drop-on-mismatch behaviour exactly as before.

---

## `xi::kv()` — persistent typed key-value state (the post-Record shape)

The successor of `xi::state()` (decision record:
`docs/new_gen/16-script-state-shape.md`). A **flat, typed, mutable key-value
store**: scalar slots (`i64 / f64 / bool / str / bin`) plus an `mp` slot
holding one nested **canonical msgpack** value for rebuilt-each-frame
structures. It crosses the host boundary as canonical msgpack bytes — one
codec, byte-deterministic (sorted keys), no JSON anywhere on the path.

```cpp
#include <xi/xi.hpp>          // umbrella includes xi_kv.hpp

XI_INSPECT_ENTRY(t, frame) {
    std::lock_guard<std::mutex> lk(xi::kv_mutex());  // needed only with xi::async
    long long n = xi::kv().get_i64("count", 0) + 1;
    xi::kv().set_i64("count", n);
    xi::kv().set_str("last_serial", "A-1042");

    xi::mp::Writer pts;                    // nested structure, rebuilt per frame
    pts.array(1); pts.map(2);
    pts.key("x"); pts.int_(3); pts.key("y"); pts.int_(4);
    xi::kv().set_mp("prev_centroids", pts);
}
```

Typed getters return the default on absent/wrong-type (`get_i64(key, def)`,
…); `has()` / `type_of()` are the strict path; `get_mp()` hands back the
canonical bytes for `xi::mp::Reader`. `set_mp` refuses malformed /
ext-bearing / duplicate-keyed bytes (same canonical gate as
`ScriptPackBuilder::add_mp`).

Same survival rules as `xi::state()`: rides hot reloads in memory (host
exports `xi_script_kv_get` / `xi_script_kv_set` — byte-length convention,
never NUL-terminated), does NOT survive backend restarts or project switches.

### Schema versioning + migration (kv flavour)

```cpp
XI_KV_SCHEMA(2);              // this script's kv shape is version 2
                              // (read back by xi_script_kv_schema_version)
```

On a version mismatch at reload the host **drops** the store
(`event:state_dropped` with `"store":"kv"`) unless the new DLL registered a
**typed** migrator — no JSON string-wrangling, you get the old store already
parsed (the host calls the script's `xi_script_kv_change` export):

```cpp
namespace { static int _mig = [] {
    xi::set_kv_migrate([](const xi::Kv& old, int from, int to)
                           -> std::optional<xi::Kv> {
        xi::Kv out;
        out.set_i64("frames", old.get_i64("count", 0));   // rename, carry value
        out.set_i64("migrated_from", from);
        return out;               // std::nullopt to DECLINE -> host drops
    });
    return 0;
}(); }
```

On success the host restores the migrated store and emits
`event:state_migrated` with `"store":"kv"`. Live proof: `examples/qa_kv_reload`.

### Porting from `xi::state()` (the bilingual window)

Both stores are live in the same DLL until the cut, so a porting script
self-seeds once — no host conversion exists, by design:

```cpp
if (xi::kv().empty() && xi::state().has("count"))
    xi::kv().set_i64("count", xi::state()["count"].as_int(0));
```

After one reload the kv channel carries and the `xi::state()` usage can be
deleted.

---

## Triggers / multi-camera

For multi-camera setups, a "gathering" plugin captures from N cameras and emits
**one record carrying N named images**; the host dispatches one inspect call per
record. There is no bus correlation step — the gathering plugin decides what
makes up a complete frame set before it emits.

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
each `emit_record()` from a source dispatches one inspect call AND a
wall-clock timer dispatches one per frame regardless of whether a
record arrived. The timer-driven dispatches arrive with no record
attached (`is_active() == false`); without the guard your script
would null-deref / read empty Images on those ticks. In single-shot
mode (`cmd:run`) this distinction doesn't apply — there's exactly one
dispatch per command.

See the `synced_stereo` reference plugin for a worked gathering source.

### `xi::Trigger` accessors

| Accessor                  | Returns                                                               |
| ------------------------- | --------------------------------------------------------------------- |
| `t.is_active()`           | `false` for synthetic timer ticks; `true` once a real event landed    |
| `t.id()` / `t.id_string()`| 128-bit trigger id (struct or 32-char hex)                            |
| `t.timestamp_us()`        | μs since Unix epoch when the source called `host->emit_record`        |
| `t.dequeued_at_us()`      | μs (same clock) when the dispatcher worker popped this event          |
| `t.image(name)`           | the named image in the record, zero-copy view (a single-image record resolves under `"frame"`, its source name, or any key) |
| `t.sources()`             | list of image names present in this record                            |
| `t.has_source(name)`      | `true` if `name` appears in `sources()`; routing without manual hash  |
| `t.meta()`                | routing/context metadata the source attached, as a read-only `Record` (empty if none) |

#### Reading trigger metadata

When a source emits with `xi::emit_record` (ABI v6) it attaches a JSON
metadata object — a command id, recipe, lane hint, whatever the line needs to
route the run. Read it back, correlated to the frame, with `t.meta()`:

```cpp
auto t = xi::current_trigger();
if (!t.is_active()) return;

auto m = t.meta();                          // borrowed read-only Record (zero-copy)
std::string cmd = m["command"].as_string(); // routing key the source set
int recipe      = m["recipe"].as_int(-1);
```

`meta()` is total — an event with no metadata (a source that emitted only a
frame, or a timer tick) returns an empty `Record`, so the reads just yield their defaults. The
doc is borrowed from the host for the life of the dispatch (zero-serialize); a
mutation copy-on-writes into the script's own doc. This is the supported way to
carry per-trigger routing context — no side-channel queue to keep in lock-step.

#### Routing by image name

Multi-source scripts often need different processing per image. The record
carries each image under the name the source gave it, so branch on
`has_source(name)`:

```cpp
auto t = xi::current_trigger();
if (!t.is_active()) return;

if (t.has_source("camera_left")) {
    auto img = t.image("camera_left");
    // ...
}
```

A gathering plugin chooses the names it emits, so the script and the source
agree on the keys without any host-side correlation policy.

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
double queue_wait_us = (double)(t.dequeued_at_us() - t.timestamp_us());  // grows during surge
double inspect_us    = (double)(now              - t.dequeued_at_us());  // your code's actual cost
// surface these however you like — a log line, the `expose` plugin, a custom
// comm plugin — since VAR no longer ships values to a viewer.
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

### Pack pilot — `t.pack()` (EXPERIMENTAL, wave-2)

> **Experimental — subject to change.** This is a polaris2 wave-2 *pilot*
> surface (docs/new_gen/08 Wave 2), the minimum needed to drive the v3
> Pack data plane end to end from a script. The final script Pack API is
> a later decision; treat everything here as unstable and opt-in.

A pack-capable **source** (e.g. `mock_camera` with config `pack_mode:
true`) emits its output not as a `Record` but as a **v3 Pack** on the
`xi.pack@1` data plane — one uniform `key → (type, bytes)` table with no
image/metadata split (docs/new_gen/07). When such a source drives your
script, read the pack back with `t.pack()`:

```cpp
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>

XI_INSPECT_ENTRY(t, frame) {
    if (!t.is_active()) return;

    auto f = t.pack();                       // borrowed, read-only view
    if (!f) return;                          // no pack on this event → NA (not a crash)

    int64_t seq = f.get_i64("seq").value_or(-1);   // typed reads, std::nullopt on miss
    if (auto img = f.get_image("frame")) {         // descriptor + zero-copy pixels
        if (seq >= 0 && img->channels == 3) xi::ok(1);
        else                                xi::ng(1, "malformed frame");
    }
}
```

`t.pack()` returns an empty view — `bool`-false, every getter
`std::nullopt` — when the event carried no pack (a normal `Record`-era
source, the common case), when the host publishes no pack plane, or on
the legacy `xi_inspect_entry(int)` entry. **The pilot rides the explicit
`XI_INSPECT_ENTRY(t, frame)` entry** (§ the entry point). Absent-pack is
fail-loud in your hands, never a fault — mirror `t.image()`'s empty-Image
behaviour and always check `if (f)`.

Getters (all borrowed, valid for the life of the pack view): `get_i64`,
`get_f64`, `get_str`, `get_bin`, `get_image`, `get_mp`, plus `count()`,
`tag_of(key)`, and a generic `for_each([](std::string_view key, int32_t
tag){ … })` walk. A returned `ScriptPack` holds its own ref on the
pack, so it is cheap to copy and **safe to capture by value into
`xi::async` / `xi::parallel_for`** — same discipline as an `xi::Image` or
a trigger snapshot.

For a **declared keyset**, `f.typed<Schema>()` gives a compile-time-checked
view where a bad slot is a *compile error* (the key is fixed at compile
time; the read is still by key string through the door — the cross-DLL
reality, docs/new_gen/08):

```cpp
struct CamPack {
    static constexpr std::array<std::string_view, 2> keys = { "seq", "frame" };
    enum { kSeq, kFrame };
};
auto tf  = t.pack().typed<CamPack>();
auto seq = tf.get_i64<CamPack::kSeq>();
```

**Pushing the pack onward (gate P2: expose-from-script).** A script can
push a pack it holds to a pack-door **sink** — no intervening plugin:

```cpp
XI_INSPECT_ENTRY(t, frame) {
    if (auto f = t.pack()) xi::use("expose").push(f);   // zero-copy, as-is
}
```

`push()` is fire-and-forget: a sink target is staged and flushed after the
inspect in frame order (the same ordered-emit discipline as
`use(sink).process(rec)`); the ack is dropped. The sealed pack crosses the
seam **untouched** — no re-encode, no `$seq` stamping — so expose's XEX1-v2
dump of a pushed pack is byte-identical to a host-side dump of the same
pack (`plugins/expose_script_push_test.cpp` asserts this). Routing comes
from the pack's own `$channel`/`$seq` entries; without them it lands on
channel `"default"` with seq 0. Returns `false` on an older host, an empty
pack, a missing instance/door, or a quarantined instance.

**What is *not* here.** *Chaining* through a plugin's pack door (pack in →
pack **out** through `xi::use(...)`, reading the reply) is still not wired
— that chaining stays host-mock-tested in `plugins/pack_pilot_test.cpp`.
Runnable end to end in `examples/pack_pilot/` (and the
`examples/qa_pack_pilot/` regression).

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

`drop_oldest` and `drop_newest` are the only accepted values. (`block` —
back-pressuring the source — was removed because it could deadlock a source that
can't drain; any other value warns to stderr and falls back to `drop_oldest`.)

`max_inflight` (default 64) bounds the **trigger-driven one-shot** path — the
non-continuous dispatch a source drives before `cmd:start`, where each emit
launches a detached inspect that serializes on the run lock. `queue_depth` bounds
the *continuous* lane; `max_inflight` bounds this *other* path so a source that
out-runs the serialized drain can't accumulate unbounded in-flight inspects (each
pinning image + meta refs). At the cap a new one-shot is **dropped-newest**, its
frame refs released, and an `XI_SYS_DROPPED` marker emitted — same bounded
semantics as the lane overflow above. `< 1`/absent → the default (**not**
unlimited); capped at 10000.

```json
{ "parallelism": { "max_inflight": 64 } }
```

`result_order` controls how per-frame results land on the wire under N > 1:

- **`completion`** (default): emit as each worker finishes — lowest latency,
  but with uneven inspect times the stream is out of frame order (sort
  client-side by `run_id` if you care).
- **`arrival`**: emit in frame-arrival order. A worker that finishes early
  waits its turn before emitting, so the `run_result`/`run_finished` stream
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
[`../internals/dispatch.md`](../internals/dispatch.md).

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
without throttling the rest. The three modes are: serialized (non-reentrant → 1),
concurrent (reentrant → N), and capped (reentrant + `max_concurrency: 1` → 1). The
non-reentrant-serialized vs reentrant-races split is proven by
`backend/tests/test_set_def_race.cpp`; `examples/qa_reentrancy/` demonstrates all
three end-to-end under a live 4-thread dispatch pool (a probe plugin reports the
max concurrency it observed per instance).

**Other caveats once N > 1 (your responsibility):**

- **`xi::state()`** is a single shared dict. Concurrent reads/writes
  race. Wrap mutations in your own `std::mutex`, or design the
  pipeline so only one thread writes a given key.
- **Reentrant plugins** (those that opted in above) must themselves be
  thread-safe: cv:: ops on pool-backed Images are mostly fine; member
  counters / caches are not — guard them with atomics or a mutex.
- **Watchdog now covers every worker** (it tracks a deadline slot per
  in-flight inspect, not a single slot). On a deadline breach it asks the
  script to cancel cooperatively. The cancel is **epoch-scoped**: it targets
  only the inspects already in flight when the watchdog tripped, so under
  N > 1 it aborts every *currently-running* frame that round (the intended
  "something's wedged, bail" signal) — but a **fresh frame the pool starts
  during the 1000 ms grace is *not* cancelled** (it would only re-run and
  abort for nothing). Healthy workers re-run next tick. If the targeted
  inspect ignores cooperative cancel, the backend **exits** so the FE
  supervisor respawns a clean one — it does **not** force-kill a worker
  (that would leak the per-instance lock). Long ops should poll
  `xi::cancellation_requested()` so a cooperative cancel takes.
- **`run_result` events** arrive interleaved across run_ids in the default
  `result_order: "completion"`. Set `result_order: "arrival"` (above) for an
  in-order wire stream, or sort client-side by `run_id`.
- **`xi::Param<T>`** reads are atomic and safe.

When in doubt, leave `dispatch_threads` at 1.

---

## Splitting a script across files

When a script grows — e.g. one block of logic per lane — split it into multiple
**headers** and `#include` them into `inspect.cpp`. Everything works in those
headers exactly as inline: `VAR()`, `xi::use()`, `xi::status()`, `state()`. See
[`examples/multi_file_script`](../../examples/multi_file_script).

**Naming convention:** the script-side files share the script's stem — the
primary `inspect.cpp` plus `inspect_*.hpp` siblings in the same folder. The
extension associates files by that prefix, so saving any `inspect_*.hpp`
recompiles the whole script (and an unrelated header dropped in the folder
doesn't). If your script is named `foo.cpp`, the prefix is `foo_`.

```
my_project/
  inspect.cpp           // #include "inspect_lane_a.hpp" + "inspect_lane_b.hpp"; calls them
  inspect_lane_a.hpp    // inline run_lane_a(int frame) { VAR(...); xi::use("..."); }
  inspect_lane_b.hpp    // inline run_lane_b(int frame) { ... }
```

```cpp
// inspect.cpp
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include "inspect_lane_a.hpp"
#include "inspect_lane_b.hpp"

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    run_lane_a(frame);
    run_lane_b(frame);
}
```

Saving **any** `inspect*` file recompiles the whole script, and hover / `VAR`
/ `xi::use("…")` underlines work in every one of them.

**Why headers, not separate `.cpp` files?** A script compiles as a single
translation unit. The script-support thunks and the `xi::use()` callback globals
are force-included into that one TU; `xi_use.hpp` reaches them with `extern`
declarations that only resolve **within** that TU. A second `.cpp` would fail to
link (unresolved `xi::use()` globals) — or, if you force-included the support
header into it too, duplicate the exported thunks. Headers `#include`d into the
one TU sidestep both. (Plugins are different — they export a C ABI and *do* use
multiple `.cpp` files; a script can't follow that model for this reason.)

A header that genuinely is a *reusable processing stage* (its own params, state,
UI) wants to be a **plugin**, not a script header — see the discussion in
[`write-a-plugin.md`](./write-a-plugin.md).

---

## Using an external library / DLL

Two ways to pull a third-party SDK into a script:

**Best for anything reusable — wrap it in a plugin.** Plugins can ship their
dependency DLLs in their own folder and get a config UI; the script just
`xi::use()`s them. See [`write-a-plugin.md`](./write-a-plugin.md).

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
  crash report + minidump (see [`debug.md`](./debug.md)).
- **`xi::Param<T>` declared inside `xi_inspect_entry`** — won't be
  registered. They must be at file scope.

---

## Where to look next

- [`docs/reference/c-abi.md`](../reference/c-abi.md) — the C ABI
  the script's plugins consume.
- [`docs/reference/ws-protocol.md`](../reference/ws-protocol.md) — the WS commands a UI client
  sends to drive a script (`run` / `set_param` / `compile_and_load` /
  …).
- [`examples/`](../../examples/) — working scripts:
  `defect_detection.cpp`, `use_demo.cpp`, `user_with_instance.cpp`.

---

## Appendix: legacy `VAR`/`EMIT` (compatibility)

> **You do not need this for new scripts.** Prefer `XI_INSPECT_ENTRY` (top of
> this guide) and surface output through the `expose` plugin. This appendix
> exists only to explain what old scripts that still contain `VAR`/`EMIT` do.

`VAR` / `EMIT` still **compile** so old scripts keep building, but they publish
nothing — `VAR(name, expr)` expands to roughly `auto name = expr;` (a plain local
declaration) and `EMIT(name)` is a bare reference. Because `VAR` *declares*, you
**cannot** `VAR(count, count)` over an existing value or `VAR(count, …)` twice in
one scope (cl.exe fires C2374; the backend appends a *"duplicate VAR(count) … use
EMIT"* hint). None of this surfaces anything to a UI anymore — for that, use the
`expose` plugin. New scripts have no reason to use `VAR`/`EMIT` at all.

The legacy entry signature `XI_SCRIPT_EXPORT void xi_inspect_entry(int frame)`
paired with `xi::current_trigger()` also still works (the host falls back to it
when the `XI_INSPECT_ENTRY`-generated symbol is absent) — see
[The entry point](#the-entry-point--xi_inspect_entry-preferred-vs-the-legacy-signature)
above. New scripts should use `XI_INSPECT_ENTRY(t, frame)` instead.
