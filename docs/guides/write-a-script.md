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
#include <xi/xi.hpp>           // xi::Image, xi::Param, xi::kv (OpenCV-free umbrella)
#include <xi/xi_cv.hpp>        // OpenCV interop — needed because we call cv:: below
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>   // xi::ScriptPackBuilder — build a pack to hand on
#include <xi/xi_result.hpp>    // xi::ok / xi::ng — the per-run verdict

// File-scope: parameters tunable from the UI without recompile.
xi::Param<int>    thresh{ "threshold", 128, {0, 255} };
xi::Param<double> sigma { "sigma",     2.0, {0.1, 10.0} };

// Explicit-trigger entry. The host passes the trigger in as `t`, so it's
// self-contained — no ambient thread_local state. `frame` is the id.
XI_INSPECT_ENTRY(t, frame) {
    (void)frame;
    if (!t.is_active()) return;         // skip synthetic timer ticks
    auto f = t.pack();                  // the sealed pack a source emitted
    if (!f) return;                     // no pack on this event → NA, not a crash
    auto in = f.get_image("frame");     // std::optional<ScriptPackImage>
    if (!in) return;                    // {width,height,channels,pixels span}

    // Image ops: call cv:: directly. Wrap the pack image's zero-copy pixel span
    // in a cv::Mat view (no copy); for outputs you want the next plugin to
    // consume, write into a fresh pool-backed xi::Image.
    cv::Mat src(in->height, in->width, in->channels == 1 ? CV_8UC1 : CV_8UC3,
                const_cast<uint8_t*>(in->pixels.data()));
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

    // Chain into a plugin's pack door: build a pack, seal it, drive the door.
    xi::ScriptPackBuilder b;
    b.add_image("gray", blur);
    b.add_i64("thresh", (int)thresh);                // slider value, no recompile
    auto result = xi::use("detector0").process(b.seal());
    if (result.is_fault()) { xi::ng(1, "detector0 fault"); return; }

    int64_t blob_count = result.get_i64("blob_count").value_or(0);

    // Surface per-run values/images to a UI via the `expose` sink (this
    // replaces the old VAR path). Requires an `expose` instance in project.json.
    xi::ScriptPackBuilder out;
    out.add_str("$channel", "inspection");
    out.add_i64("$seq", (int64_t)xi::run_id());       // producer-stamped ordering
    out.add_image("input", in->width, in->height, in->channels, in->pixels.data());
    out.add_i64("blob_count", blob_count);
    xi::use("expose").push(out.seal());

    if (blob_count <= 3) xi::ok(1, "clean");          // the run's verdict
    else                 xi::ng(1, "too many blobs");
}
```

That's a full script. A few constructs do the heavy lifting:

| Primitive | Purpose | Lifetime |
|---|---|---|
| `xi::use("name")` → `xi::UseProxy&` | Proxy to a backend-managed instance (camera, model, etc.) | Instance lives across hot-reloads, persisted by host |
| `xi::Param<T>` | Tunable scalar with UI slider | Per script DLL, restored from `project.json` on reload |
| `xi::use("expose").push(pack)` | Surface per-run values/images to a UI (replaces VAR) — see [below](#surfacing-output--the-expose-plugin) | Per `inspect_entry` invocation |
| `t.pack()` → `xi::ScriptPack` | The sealed pack the trigger's source emitted — the frame + its metadata | Borrowed for the dispatch; the ScriptPack holds its own ref |

> **Output goes through the `expose` plugin.** The old VAR value-tracking and
> the `vars` wire message were **removed** from core well before the v12 cut,
> and the in-core JPEG preview encoder left at THE CUT (it lives in the
> `imgcodec` capability provider now). To show per-run values/images in a UI,
> build a pack with
> `xi::ScriptPackBuilder`, tag it with a channel, and push it to the **`expose`
> plugin** with `xi::use("expose").push(pack)` — the shipped data-out surface,
> documented [below](#surfacing-output--the-expose-plugin). The run's pass/fail
> verdict still leaves via `xi::result(...)` (also below).

Plus:
- `xi::ScriptPack` / `xi::ScriptPackBuilder` — the universal data container (one
  keyed, typed table: images + scalars + nested msgpack).
- `xi::async(fn, args...)` — parallel ops (returns `Future<R>`).
- `xi::kv()` — persistent typed key-value store that survives hot-reloads.

---

## The entry point — `XI_INSPECT_ENTRY` (preferred) vs the legacy signature

There are **two** ways to declare the entry, and new scripts should use the first:

```cpp
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

// PREFERRED (script-entry ABI v2, "A4"): the host hands you the trigger EXPLICITLY.
XI_INSPECT_ENTRY(t, frame) {          // `t` is a xi::Trigger, `frame` is int
    if (!t.is_active()) return;
    auto f = t.pack();                // the source's sealed pack
    if (!f) return;
    // ... inspect f.get_image("frame") / f.get_i64(...) ...
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
`xi::Trigger t` from a host-filled view — the event's pack handle + image handles +
the host API to resolve them all travel *in the argument*, with **no ambient
`thread_local` seam**. Two concrete wins over `xi::current_trigger()`:

- **`t` is valid on ANY thread** and is **safe to capture by value** into
  `xi::async` / `xi::parallel_for` — it is exactly the `trigger_snapshot()` you
  otherwise had to make by hand (see [Parallelism safety](#parallelism-safety--three-rules-for-worker-threads),
  rule 1). No accidental cross-thread read of the ambient trigger.
- The `t.*` accessor table below is **identical** for both entries — only *how you
  obtain `t`* differs. Everything else in this guide (`xi::use`, `xi::Param`,
  `expose`, `xi::result`, `xi::kv`) is unchanged.

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
              └─ on next save: ─→ kv_get() (mp bytes) ─→ unload DLL ─→
                 load new DLL ─→ kv_set(mp bytes) ─→ restore params ─→ ready
```

State that survives the reload:
- `xi::kv()` canonical-mp bytes (persisted by `xi_script_kv_get` /
  `xi_script_kv_set` — the sole cross-frame state channel, see *`xi::kv()`* below).
- `xi::Param<T>` values (replayed by `xi_script_set_param`).

State that does NOT survive:
- Static / global C++ objects in your script (the DLL is unloaded).
- Anything you stored in plain heap.

If you need persistence, write to `xi::kv()`.

---

## `xi::use` — calling plugins

The host owns instances; the script proxies to them via `xi::UseProxy`.

```cpp
auto& det = xi::use("detector0");
xi::ScriptPackBuilder b;
b.add_image("gray", img);
b.add_i64("t", 50);
auto out = det.process(b.seal());       // drives det's xi.pack@1 door
```

`UseProxy` exposes `process(ScriptPack)`, `push(ScriptPack)`, and
`exchange(string)`. `process()` is the request-reply chain — it returns the
door's reply pack; `push()` feeds an ordered **sink** fire-and-forget (see
[expose](#surfacing-output--the-expose-plugin)).

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
API. A source seals a pack and emits it (a pack-mode source emits a sealed pack),
and the script reads the resulting frame from `t.pack()` (see *Triggers /
multi-camera* below), not by polling the source proxy.

---

## `xi::Param<T>` — tunable parameters

Declared at file scope. Host syncs them to the UI panel and persists in
`project.json`.

```cpp
xi::Param<int>    thresh { "threshold", 128, { 0, 255 }   };
xi::Param<double> sigma  { "sigma",     2.0, { 0.1, 10.0 } };
xi::Param<bool>   invert { "invert",    false             };

XI_INSPECT_ENTRY(t, frame) {
    int th = thresh;           // implicit cast to T
    if (invert) {/*…*/}
}
```

When the user drags a slider, `cmd:set_param` updates the value; the
next `cmd:run` picks up the new value. No compile.

`xi::kv()` is flat and typed, so it is not the place for richer config. For
nested/structured config (arrays, sub-objects), carry it as a nested `add_mp`
subtree in a pack, or expose it through a plugin's `get_def` / `set_def`.

---

## Output channels at a glance — which one do I use?

Historically a script had six-plus ways to emit something (VAR, EMIT, result,
status, expose, emit_binary). That is now **two blessed surfaces** plus one
operator-status side-channel. When you ask *"how do I show this value?"* the
answer is almost always **expose**:

| I want to… | Use | What it is |
|---|---|---|
| Ship the run's **pass/fail verdict** (one per run) | `xi::result(code,msg)` / `xi::ok(...)` / `xi::ng(...)` | The single verdict record MES/PLC/HMI consume. Exactly one per run, last-write-wins. |
| Show **any other value or image** (scores, counts, previews, debug channels) | `xi::use("expose").push(pack)` | The official observational data-out surface. Build a pack with `xi::ScriptPackBuilder`, tag `"$channel"`, push. |
| Set the operator-facing **"what am I doing right now"** string | `xi::status("...")` | One sticky, human, last-write-wins status line (`xi_status.hpp`). Not a value, not a log. |
| Write a diagnostic **log line** | `host->log(...)` / plugin `log_*` | Event stream for debugging, not UI values. |

Everything else is legacy: **`VAR` / `EMIT` were removed** (they no longer
compile — `C3861` — see below), and the old
per-key `VAR`/`vars`/JPEG-preview wire paths are gone. There is **no** author-facing
`emit_binary` verb to reach for — image output rides on the `expose` pack.

## Surfacing output — the `expose` plugin

VAR used to ship per-run values/images to a viewer panel; that path was removed
from core. The shipped replacement is the **`expose` plugin** — the official
script data-out surface: build a pack with `xi::ScriptPackBuilder`, tag it with a
**channel id**, and push it. A UI tabs between channels — per stage / per thread /
per camera. There is **no special header and no macro** — `expose` is a declared
**sink** you feed via the generic `xi::use("expose").push(pack)`:

```cpp
XI_INSPECT_ENTRY(t, frame) {
    // ... compute img, score ...

    xi::ScriptPackBuilder r;
    r.add_str("$channel", "bright");            // channel id (string) — reserved key
    r.add_i64("$seq", (int64_t)xi::run_id());   // ordering identity, producer-stamped
    r.add_i64("frame", frame);                  // a value
    r.add_f64("score", score);                  // a value
    r.add_image("edges", img);                  // an image, tagged by key

    xi::use("expose").push(r.seal());           // fire-and-forget sink feed
}
```

- **The pack *is* the payload.** Build it with `xi::ScriptPackBuilder`
  (`add_i64`/`add_f64`/`add_str`/… for values, `add_image(...)` for images).
  **Display order = the pack's own key order** — insertion order is preserved, so
  fields render top-to-bottom in the order you wrote them. No `__LINE__` /
  `$layout` machinery.
- **`"$channel"`** (reserved key, a string) selects the output channel; it is
  **created implicitly** on first send — no pre-declaration. Stamp **`"$seq"`**
  yourself with `xi::run_id()` before you seal — the host never stamps a sealed
  pack (it is immutable), and `$seq` is the ordering identity `expose` reads.
  Multiple channels per run is fine — build another pack with a different
  `"$channel"` and `push()` again.
- Attaching the **same image buffer** under two keys is cheap: the preview
  encoder (the `xi.jpeg.encode` capability) compresses it once (dedup).

> **`expose` is an ordered output sink.** Its `plugin.json` declares
> `"sink": true`, so under parallel dispatch (`parallelism.dispatch_threads > 1`)
> the host **stages** each `use("expose").push(...)` and flushes it in
> **frame-arrival order** instead of worker-completion order. The staged flush
> guarantees delivery order; the pack's own producer-stamped `$seq` carries the
> ordering identity. So live output never tears or reorders across workers, with no
> extra work in your script. Any plugin you want frame-ordered the same way just
> sets `"sink": true` — see [`../reference/c-abi.md`](../reference/c-abi.md).
>
> Feeding a sink is `push()`, not `process()`: calling `process()` on a declared
> ordered sink is **rejected fail-loud** (an empty pack + a once-per-name log
> naming the fix), because a request-reply on a staged sink could only ever return
> an empty reply. `process()` is the reply chain; `push()` is the sink feed.

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

XI_INSPECT_ENTRY(t, frame) {
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

The host still hands the script a `frame_path` per `cmd:run` (the Python
SDK's `c.run(frame_path=...)`), readable via `xi::current_frame_path()`:

```cpp
#include <xi/xi.hpp>

XI_INSPECT_ENTRY(t, frame) {
    if (auto f = t.pack()) {                        // decoded frame rides the pack
        if (auto img = f.get_image("frame")) { /* ... pipeline ... */ }
    }
}
```

There is **no in-script decode** — the old `xi::Image` decode call was removed
at THE CUT. Turning a file path into pixels is the `xi.image.decode`
capability's job (provided by an `imgcodec` instance in the project, or a
machine-wide `--autoload-lib` provider). For a `cmd:run` with a `frame_path`,
the **host** does this for you: it decodes the file through the capability and
injects the image into a one-shot sealed pack under the key `"frame"` (plus any
top-level scalar `meta` entries) — so your script's code path is identical to a
live camera: read `t.pack()`. With no decode provider in the deployment the
frame is simply not injected (the trigger pack has no `"frame"` entry).

If the run was started with no `frame_path` arg, `current_frame_path()`
returns an empty string. Supported formats (PNG / JPEG / BMP / TGA / GIF / PSD /
HDR / PIC) are whatever the `imgcodec` provider decodes; the decode engine lives
in the plugin, not the script DLL.

(To view a frame back in a UI, push a pack carrying it to the `expose` plugin
with `r.add_image("name", ...)` — see
[Surfacing output](#surfacing-output--the-expose-plugin).)

---

## `pack` — the universal container

One sealed, keyed, typed table — images, scalars, strings, binary, and nested
msgpack subtrees all live in the same `key → (type, bytes)` map, with no
image/metadata split. It is the input + output of a plugin's pack door, the
trigger's payload (`t.pack()`), and what you push to a sink. **Read** it as a
`xi::ScriptPack` (from `t.pack()` or a door reply); **build** one with
`xi::ScriptPackBuilder`.

```cpp
#include <xi/xi_script_pack.hpp>

// ---- read (a ScriptPack: t.pack(), or a door reply) --------------------------
auto f = t.pack();
if (!f) return;                                    // operator bool == valid()
if (f.is_fault()) { xi::ng(1, "poisoned input"); return; }  // check BEFORE reading
int64_t n  = f.get_i64("count").value_or(0);       // typed, std::optional, nullopt on miss
bool    ok = f.get_bool("pass").value_or(false);
auto    im = f.get_image("binary");                // std::optional<ScriptPackImage>
auto    lbl= f.get_str("label");                   // std::optional<string_view>

// Nested tree (ONE canonical-msgpack subtree, read with xi::mp::Reader):
if (auto pts = f.get_mp("points")) {
    xi::mp::Reader rd(pts->data(), pts->size());
    // ... walk the array/map ...
}

// ---- build (a ScriptPackBuilder → seal into a first-class ScriptPack) --------
xi::ScriptPackBuilder b;
b.add_i64("count", 5);
b.add_bool("pass", true);
b.add_image("binary", img);
xi::mp::Writer roi;                                // one nested subtree
roi.map(2); roi.key("x"); roi.int_(3); roi.key("y"); roi.int_(4);
b.add_mp("roi", roi);
auto out = b.seal();                               // empty ScriptPack if a rule was violated
```

Every `add_*` returns `bool` (accepted); a violated pack rule makes `seal()`
return an empty ScriptPack — never a crash. Reserved `$`-prefixed keys carry
routing and provenance: `$channel` (sink routing), `$seq` (ordering),
`$fault`/`$fault_key`/`$fault_detail` (the fail-loud error path — `is_fault()`,
`fault_reason()`), `$src`/`$prov` (producer + hop chain). A sealed ScriptPack
holds its own ref on the host pack, so it is safe to hold past the dispatch and
**capture by value into `xi::async` / `xi::parallel_for`**. Enumerate an unknown
pack with `f.for_each([](std::string_view key, int32_t tag){ … })`, or read a
declared keyset compile-checked with `f.typed<Schema>()`.

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

1. **Read the trigger's pack on the inspect thread; capture it by value into
   parallel regions.** The explicit `XI_INSPECT_ENTRY(t, frame)` trigger `t` and
   the `xi::ScriptPack` from `t.pack()` are **self-contained** — a ScriptPack holds
   its own ref on the pack (and its pool buffers), so capturing it by value is safe
   on any worker thread. (The legacy ambient `xi::current_trigger()` thunks read
   thread-local host state and are valid *only* on the inspect thread — a worker
   call fails loud; the pack path has no such seam.) Read once, capture by value:

   ```cpp
   auto f = t.pack();                                  // inspect thread
   auto img = f.get_image("gray");
   xi::parallel_for(rows, [&, f](int y){               // any worker thread
       // f is valid here — the ScriptPack copy keeps the pack + pixels alive
       process_row(img->pixels, y);
   });
   ```

   The ScriptPack copy keeps the pack's pool buffers alive for the whole region;
   its accessors touch no thread-local.

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

auto f = t.pack();        // read on the inspect thread
xi::parallel_for(h, [&, f](int y) {
    // runs across the OpenMP pool; faults here are caught and rethrown
    // on the inspect thread, not propagated out of the omp region.
    process_row(y);
});
```

It installs the SEH translator on each worker, polls
`xi::cancellation_requested()` and skips remaining iterations when the
enclosing task's cancel token is flipped (`Future::cancel()` / a dropped
unconsumed `Future`), catches every fault inside the region and rethrows the
first one on the inspect thread, and re-installs the inspect-thread image-pool
owner per worker so parallel-created images stay attributed. `xi::async` already
does the same (SEH + owner propagation) for independent-branch fan-out.

> **Per-run identity is *not* one of these hazards.** `xi::run_id()`,
> `xi::current_frame_path()` and `xi::result()` read the host's explicit
> per-run **RunContext**, which `xi::async` / `xi::parallel_for` /
> `xi::spawn_worker` all propagate — they are correct on any xi-spawned worker,
> and a verdict set from an `xi::async` / `xi::parallel_for` worker **reaches
> the run** (it is no longer silently dropped). A `xi::spawn_worker` worker gets
> its own by-value snapshot: `run_id()` / `current_frame_path()` stay valid even
> if the worker outlives the inspect, while its `xi::result()` is a deliberate
> no-op (a detached worker can't verdict a finished run). The explicit trigger
> also carries the identity self-contained: `t.run_id()` / `t.frame_path()`.
> Off a run entirely, the free functions fail loud — Debug aborts; Release
> warns once and returns the `0` / `""` sentinel. Needs `/openmp`
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

## `xi::kv()` — persistent typed key-value state

The single cross-frame state channel: a **flat, typed, mutable key-value store**
— scalar slots (`i64 / f64 / bool / str / bin`) plus an `mp` slot holding one
nested **canonical msgpack** value for rebuilt-each-frame structures. It lives
SDK-side and crosses the host boundary as canonical msgpack bytes — one codec,
byte-deterministic (sorted keys), no JSON anywhere on the path (decision record:
`docs/new_gen/16-script-state-shape.md`).

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

Typed getters take a default and return it on absent/wrong-type
(`get_i64(key, def)`, …); `has()` / `type_of()` are the strict path; `get_mp()`
hands back the canonical bytes for `xi::mp::Reader`. `set_mp` refuses malformed /
ext-bearing / duplicate-keyed bytes (the same canonical gate as
`ScriptPackBuilder::add_mp`). Use it for cross-frame counters, calibration
results, "have we seen this serial number" caches.

Survives:
- Hot-reload (DLL unload + reload) — the host captures the store from the old DLL
  and restores it into the new one, **in memory** (host exports
  `xi_script_kv_get` / `xi_script_kv_set` — byte-length convention, never
  NUL-terminated).

Does NOT survive (deliberately — the project boundary reset is pinned by
`qa_param_state_isolation`):
- Backend restart (the store is never written to disk).
- Project open/close (a fresh project starts from its own defaults).

### Schema versioning + migration across a code change

By default a hot-reload that **changes the shape** of the store would let the old
DLL's persisted bytes default-fill the new shape incorrectly. Guard against it by
declaring a **schema version** at file scope — bump it whenever the shape changes:

```cpp
XI_KV_SCHEMA(2);              // this script's kv shape is version 2
                              // (read back by xi_script_kv_schema_version)
```

On reload the host compares the saved version to the new one; on a **mismatch** it
**drops** the store (emits `event:state_dropped` carrying `"store":"kv"`) rather
than restore a mismatched shape. Version `0` (absent) means "unversioned" —
blind-restore.

If you want cross-version **continuity** instead of a drop, register a migrator —
the same discipline as Erlang's `code_change/3`. It runs in the NEW DLL (which
alone knows both shapes) and gets the old store **already parsed** into a `xi::Kv`
(no string-wrangling — the host calls the script's `xi_script_kv_change` export):

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

Register it at static-init time (it runs at DLL load, before the host's first
restore). The migrator must be **pure** w.r.t. the call — it must not read live
`xi::kv()`, which has not been restored yet. On success the host restores the
migrated store and emits `event:state_migrated` (also carrying `"store":"kv"`).
Live proof: `examples/qa_kv_reload`.

---

## Triggers / multi-camera

For multi-camera setups, a "gathering" plugin captures from N cameras and emits
**one pack carrying N named images**; the host dispatches one inspect call per
pack. There is no bus correlation step — the gathering plugin decides what
makes up a complete frame set before it emits.

Inside the script:

```cpp
XI_INSPECT_ENTRY(t, frame) {
    if (!t.is_active()) return;
    auto f = t.pack();
    if (!f) return;

    auto left  = f.get_image("cam_left");
    auto right = f.get_image("cam_right");
    if (left && right) { /* stereo_match(*left, *right) ... */ }
}
```

The `is_active()` guard is **required** in continuous mode
(`cmd:start fps=N`). The host runs two dispatch sources side-by-side:
each pack a source emits dispatches one inspect call AND a
wall-clock timer dispatches one per frame regardless of whether a
pack arrived. The timer-driven dispatches arrive with no pack
attached (`is_active() == false`, and `t.pack()` is empty); without the guard
your script would read a nullopt frame on those ticks. In single-shot
mode (`cmd:run`) this distinction doesn't apply — there's exactly one
dispatch per command.

See the `synced_stereo` reference plugin for a worked gathering source.

### `xi::Trigger` accessors

| Accessor                  | Returns                                                               |
| ------------------------- | --------------------------------------------------------------------- |
| `t.is_active()`           | `false` for synthetic timer ticks; `true` once a real event landed    |
| `t.id()` / `t.id_string()`| 128-bit trigger id (struct or 32-char hex)                            |
| `t.timestamp_us()`        | μs since Unix epoch when the source emitted this event                |
| `t.dequeued_at_us()`      | μs (same clock) when the dispatcher worker popped this event          |
| `t.sources()`             | source identity of this event (the emitting instance + the pack's `$src`) |
| `t.pack()`                | the sealed `xi::ScriptPack` this event carried (empty if none) — the frame + all its metadata |

The frame and any routing/context metadata the source attached **ride the pack**:
read them with `t.pack()` and the typed getters — there is no separate metadata
plane.

#### Reading trigger metadata

Metadata the source attached — a command id, recipe, lane hint, whatever the line
needs to route the run — rides the pack as ordinary entries. Read it back,
correlated to the frame, straight off `t.pack()`:

```cpp
XI_INSPECT_ENTRY(t, frame) {
    if (!t.is_active()) return;
    auto f = t.pack();
    if (!f) return;

    auto cmd   = f.get_str("command");          // std::optional<string_view>
    int recipe = (int)f.get_i64("recipe").value_or(-1);
}
```

Each getter is total in the sense that it yields `std::nullopt` / the default when
the key is absent (a source that emitted only a frame, or a timer tick with no
pack). This is the supported way to carry per-trigger routing context — no
side-channel queue to keep in lock-step.

#### Routing by source

Multi-source scripts often need different processing per source. A pack trigger's
source identity is the emitting instance (`t.primary_source()`) plus the pack's
own `$src` stamp; `has_source(name)` consults both:

```cpp
XI_INSPECT_ENTRY(t, frame) {
    if (!t.is_active()) return;

    if (t.has_source("camera_left")) {
        // ... route on the emitting source ...
    }
}
```

For gathering sources that pack several images under distinct keys, branch on the
image keys instead — enumerate them with `f.for_each(...)` or probe with
`f.get_image("camera_left")`. The gathering plugin chooses the keys it emits, so
the script and the source agree without any host-side correlation policy.

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
XI_INSPECT_ENTRY(t, frame) {
    if (!t.is_active()) return;

    int64_t now = xi::now_us();
    double queue_wait_us = (double)(t.dequeued_at_us() - t.timestamp_us());  // grows during surge
    double inspect_us    = (double)(now              - t.dequeued_at_us());  // your code's actual cost
    // surface these however you like — a log line, the `expose` plugin, a custom
    // comm plugin — since VAR no longer ships values to a viewer.
}
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

### The pack data plane — `t.pack()`

A **source** (e.g. `mock_camera`) emits its output as a **sealed pack** on the
`xi.pack@1` data plane — one uniform `key → (type, bytes)` table with no
image/metadata split (docs/new_gen/07). Read it back with `t.pack()`:

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
`std::nullopt` — when the event carried no pack (a synthetic timer tick), when
the host publishes no pack plane, or on the legacy ambient entry
(`xi::current_trigger()`). **The pack rides the explicit
`XI_INSPECT_ENTRY(t, frame)` entry** (§ the entry point). Absent-pack is
fail-loud in your hands, never a fault — the reads just return `std::nullopt`, so
always check `if (f)` before reading.

Getters (all borrowed, valid for the life of the pack view): `get_i64`,
`get_f64`, `get_bool`, `get_str`, `get_bin`, `get_image`, `get_mp`, plus
`count()`, `tag_of(key)`, and a generic `for_each([](std::string_view key,
int32_t tag){ … })` walk. A returned `ScriptPack` holds its own ref on the
pack, so it is cheap to copy and **safe to capture by value into
`xi::async` / `xi::parallel_for`** — same discipline as an `xi::Image`.
Before reading results off a pack that came from a door, check `f.is_fault()`
(and `f.fault_reason()`): a fault is a normal sealed pack carrying `$fault`, not
a null (doc 15).

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

**Pushing the pack onward (expose-from-script).** A script can push a pack it
holds — the trigger's own, or a `ScriptPackBuilder`-built one — to a pack-door
**sink** with no intervening plugin:

```cpp
XI_INSPECT_ENTRY(t, frame) {
    if (auto f = t.pack()) xi::use("expose").push(f);   // zero-copy, as-is
}
```

`push()` is fire-and-forget: a sink target is staged and flushed after the
inspect in frame order (the same ordered-emit discipline every sink push gets);
the ack is dropped. The sealed pack crosses the seam **untouched** — no
re-encode, no host `$seq` stamping (a sealed pack is immutable) — so expose's
XEX1-v3 dump of a pushed pack is byte-identical to a host-side dump of the same
pack (`plugins/expose_script_push_test.cpp` asserts this). Routing comes from the
pack's own `$channel`/`$seq` entries; without them it lands on channel
`"default"` with seq 0, so stamp `$seq` yourself
(`b.add_i64("$seq", (int64_t)xi::run_id())`) before you seal a pack you build.
Returns `false` on an older host, an empty pack, a missing instance/door, or a
quarantined instance.

**Chaining through a plugin's pack door.** Driving a plugin (pack in → pack
**out**, reading the reply) is `xi::use("name").process(pack)` — the request-reply
path shown under [`xi::use`](#xiuse--calling-plugins). A fault input
short-circuits host-side: the plugin never runs and you get back a fault pack
with this instance appended to `$prov`. Runnable end to end in
`examples/pack_pilot/` (and the `examples/qa_pack_pilot/` regression).

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

- **`xi::kv()`** is a single shared store. Concurrent reads/writes
  race. Wrap mutations in `xi::kv_mutex()` (or your own `std::mutex`), or design
  the pipeline so only one thread writes a given key.
- **Reentrant plugins** (those that opted in above) must themselves be
  thread-safe: cv:: ops on pool-backed Images are mostly fine; member
  counters / caches are not — guard them with atomics or a mutex.
- **Watchdog now covers every worker** (it tracks a deadline slot per
  in-flight inspect, not a single slot). It has **one phase**:
  overrun → 1000 ms grace → hard exit. A frame that exceeds its budget but
  returns during the grace produces a **normal trusted verdict** — merely-slow
  frames are never aborted. If an inspect is still wedged after the grace, the
  backend **exits** (`_Exit`) so the FE supervisor respawns a clean one — it
  does **not** force-kill a worker (that would leak the per-instance lock).
  There is no cooperative soft-cancel layer anymore: the watchdog never asks
  the script to bail, and `xi::cancellation_requested()` reads **only** the
  per-task `xi::async` cancel token (`Future::cancel()` / a dropped unconsumed
  `Future`).
- **`run_result` events** arrive interleaved across run_ids in the default
  `result_order: "completion"`. Set `result_order: "arrival"` (above) for an
  in-order wire stream, or sort client-side by `run_id`.
- **`xi::Param<T>`** reads are atomic and safe.

When in doubt, leave `dispatch_threads` at 1.

---

## Splitting a script across files

When a script grows — e.g. one block of logic per lane — split it into multiple
**headers** and `#include` them into `inspect.cpp`. Everything works in those
headers exactly as inline: `xi::use()`, `xi::status()`, `xi::kv()`, the pack
reads/builders. See
[`examples/multi_file_script`](../../examples/multi_file_script).

**Naming convention:** the script-side files share the script's stem — the
primary `inspect.cpp` plus `inspect_*.hpp` siblings in the same folder. The
extension associates files by that prefix, so saving any `inspect_*.hpp`
recompiles the whole script (and an unrelated header dropped in the folder
doesn't). If your script is named `foo.cpp`, the prefix is `foo_`.

```
my_project/
  inspect.cpp           // #include "inspect_lane_a.hpp" + "inspect_lane_b.hpp"; calls them
  inspect_lane_a.hpp    // inline run_lane_a(const xi::Trigger& t) { auto f = t.pack(); xi::use("..."); }
  inspect_lane_b.hpp    // inline run_lane_b(const xi::Trigger& t) { ... }
```

```cpp
// inspect.cpp
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include "inspect_lane_a.hpp"
#include "inspect_lane_b.hpp"

XI_INSPECT_ENTRY(t, frame) {
    run_lane_a(t);
    run_lane_b(t);
}
```

Saving **any** `inspect*` file recompiles the whole script, and hover /
`xi::use("…")` underlines work in every one of them.

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
  gone after a script reload (`compile_and_load`) or project close.
  Use `xi::kv()` for persistence.
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
> exists only to explain what happens to old scripts that still contain
> `VAR`/`EMIT`.

`VAR` / `EMIT` are **gone from the SDK headers entirely** — an old script that
still contains `VAR(name, expr)` / `EMIT(name)` **no longer compiles** (cl.exe
fires C3861, *identifier not found*). There was an interim era in which they
were kept as compile-only no-ops (`VAR(name, expr)` expanded to a plain
`auto name = expr;` declaration — which is why the backend's diagnostics still
carry a *"duplicate VAR(count)"* hint for the redefinition errors that shim
produced); that shim has since been deleted too. Port the script: delete the
`EMIT(...)` lines, turn each `VAR(name, expr)` into a plain local, and surface
whatever the UI should see through the `expose` plugin (a pack + `"$channel"` +
`xi::use("expose").push(pack)`).

The legacy entry signature `XI_SCRIPT_EXPORT void xi_inspect_entry(int frame)`
paired with `xi::current_trigger()` also still works (the host falls back to it
when the `XI_INSPECT_ENTRY`-generated symbol is absent) — see
[The entry point](#the-entry-point--xi_inspect_entry-preferred-vs-the-legacy-signature)
above. New scripts should use `XI_INSPECT_ENTRY(t, frame)` instead.
