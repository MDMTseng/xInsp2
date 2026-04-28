# Writing an inspection script

The inspection **script** is the C++ file the user authors per project
that runs once per inspection cycle. It's compiled to a DLL by the
backend's `cl.exe` driver and hot-reloaded on save.

This guide walks through the surface area an inspection author actually
uses, with pointers into the deeper reference for each piece.

---

## The shape

```cpp
#include <xi/xi.hpp>
#include <xi/xi_ops.hpp>
#include <xi/xi_use.hpp>

using namespace xi::ops;

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
    VAR(gray,  toGray(img));
    VAR(blur,  gaussian(gray, sigma));

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

> **Gotcha — `VAR(name, ...)` declares a local.** The macro expands to
> roughly `auto name = expr; <ship to viewer>`, so `name` becomes a
> real variable in the enclosing scope. You **cannot** `VAR(gray, ...)`
> if `gray` is already declared earlier — you'll get a redeclaration
> error from the compiler. Either inline the expression as the second
> arg (`VAR(gray, toGray(img))`) so the VAR macro is its own
> declaration, or use a fresh name.

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
auto fa = xi::async(toGray, img);
auto fb = xi::async(blur,  img, 2.0);
auto [g, b] = xi::await_all(fa, fb);    // tuple<Image, Image>
```

Or shorthand for an op already in the library:

```cpp
ASYNC_WRAP(gaussian)        // declared once in xi_ops.hpp / your header
auto fb = async_gaussian(img, 2.0);
```

`Future<T>` is consumed once; reuse compiles to an error.
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

See [`docs/architecture.md`](../architecture.md) for bus policies (Any
/ AllRequired / LeaderFollowers) and the `synced_stereo` reference
plugin.

---

## Common pitfalls

- **Forgetting `XI_SCRIPT_EXPORT`** on `xi_inspect_entry`. The host's
  loader will report "missing entry point" and refuse to load.
- **Holding raw pointers across reloads**. The DLL's static memory is
  gone after `unload_script`. Use `xi::state()` for persistence.
- **Stack overflow / heap corruption** still crashes the process —
  SEH translation handles segfaults / div0 / array overrun, not
  unbounded recursion or write-past-buffer-end. The
  `shm-process-isolation` spike provides a separate-process script
  runner for the strictest cases.
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
