# Plugin caveats & patterns — the non-obvious bits

A checklist of the things that bite plugin authors. Most are detailed elsewhere —
this page is the index + the key rule for each, plus the UI/architecture patterns
that don't have another home. Read it once before shipping a plugin.

Start at [`write-a-plugin.md`](./write-a-plugin.md) for the task tour and
[`../reference/c-abi.md`](../reference/c-abi.md) for the exact contract.

## Concurrency & config changes

- **Pick your plugin type (T0–T3) and know what *you* must do.** Non-reentrant
  (default) → the host serializes `set_def` vs `process`, you write nothing.
  Reentrant → concurrency is yours; a *stateless* reentrant plugin needs no lock,
  a *stateful* one must lock its own shared state. `binarize(threshold)` is **not**
  stateless — `threshold` is racy. Full table + caveats:
  [`write-a-plugin.md` → "Concurrency & config-change safety"](./write-a-plugin.md#concurrency--config-change-safety--which-plugin-type-are-you).
- **`prepare()` runs UNGATED.** If you opt into the frame-perfect swap
  (`XI_PLUGIN_STAGED`), the host calls `prepare()` *concurrent with* `process()`,
  so it must touch the **staging slot only**, never live state. `commit()` is the
  only gated half. Omit it and a config change is a plain (serialized) `set_def`.
  See [`c-abi.md` §1](../reference/c-abi.md) + `write-a-plugin.md`.
- **The lock-free escape hatch.** Want reentrant throughput without a lock? Keep
  all mutable state behind one `std::atomic<std::shared_ptr<const T>>` that
  `process()` reads, swap a fresh snapshot on change — exactly the double-slot
  shape, doing double duty.

## Memory & data across the ABI

- **Image handles are refcounted, not owned.** Input handles belong to the host
  for the `process()` call (addref to cache across calls, release in `destroy`);
  output handles transfer to the host on return — don't release them. Details:
  [`c-abi.md` → "Image pool"](../reference/c-abi.md).
- **yyjson layout must match, or opt into the slow path.** A prebuilt plugin whose
  yyjson layout differs from the host is refused at load unless `plugin.json` sets
  `"json_fallback": true` (then it runs the slow JSON path). Rebuild against the
  host's vendored yyjson to stay on the zero-copy doc path.
- **Keep `get_def` small.** Hundreds of bytes of JSON; persist big assets (model
  weights, templates, calibration) under `host->instance_folder()`, not in the def.
- **Cross-CRT free.** If you hand-roll the C record-out helpers, the plugin and
  backend must share a CRT (`/MD`, the CMake default). The C++ `XI_PLUGIN_IMPL`
  path routes output strings through plugin-owned TLS and avoids this.

## Trust model — what you DON'T have to do

- **Plugins load trusted, speed-first.** There's no cert gate and no
  hostile-plugin sandbox. Don't add hostile-plugin defenses, and don't add
  hot-path cost for safety you don't need — flag anything that slows `process()`.

## UI patterns (the part people get wrong)

These are about *where logic lives* once a plugin has a webui. The components live
in `ui-components/` (the `xi-*` web components); see
[`../roadmap/webui-and-ui-export.md`](../roadmap/webui-and-ui-export.md).

- **UX flow belongs in the webui (JS), NOT in C++.** A plugin should expose
  *capabilities* as `exchange` commands (`set_corner`, `validate`, `finalize`) and
  let the webui orchestrate the *flow* (a teach wizard: which step, what order,
  back/skip). **Never block a plugin thread waiting on a human gesture** — that
  inverts the "headless backend, UI is a client" model, ties UX to the compile
  cycle, and can't be scripted in tests. A guided wizard is JS calling your
  granular commands, not C++ driving the UI. (This is why the plugin-initiated
  "request editor" host hook was rejected — it puts UX in C++.)

- **Don't reimplement measurement geometry in JavaScript.** The classic trap:
  C++ computes the geometry (constraints, line↔circle near/far/tangent/intersection
  points), and the webui re-implements the same math in JS for live interaction —
  two copies that drift and disagree, worst when C++ uses a native constraint lib
  JS can't match. Instead:
  - Factor the geometry into a **portable, dependency-light kernel** — ideally a
    **declarative constraint graph** (`P = intersect(L,C)`, `T = tangent(C, from=P)`)
    so the *logic* is data + one solver, not two hand-written copies.
  - Compile that one kernel to **native** (for inspection) **and WASM** (for the
    webui). Same source ⇒ no drift.
  - **Native is always the authority** (it recomputes at commit / inspection); the
    WASM kernel is just the local 60 fps interactive preview.
  - **Discrete** interactions (place a point, click to intersect) don't even need
    WASM — a cheap `exchange` geometry query (ask-native) is simplest. Reach for
    WASM only when continuous drag-preview latency hurts. A native constraint lib
    that won't cross-compile stays on the ask-native path.

  This is plugin/application territory, **not core** — the framework just provides
  the rails (viewer + draw tools + `exchange` + the component library). A future
  example will demonstrate the kernel→native+WASM pattern end-to-end.

## See also

- [`write-a-plugin.md`](./write-a-plugin.md) — the task tour.
- [`../reference/c-abi.md`](../reference/c-abi.md) — the exact ABI contract.
- [`../roadmap/webui-and-ui-export.md`](../roadmap/webui-and-ui-export.md) — the UI
  component library + section-based export.
