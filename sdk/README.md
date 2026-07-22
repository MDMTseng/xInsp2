# xInsp2 Plugin SDK

Write a plugin in ~30 lines of C++. Get a live GUI, JSON+image I/O,
persistent state, and hot-reload — all without touching the host.

> **New here?** Start with [GETTING_STARTED.md](./GETTING_STARTED.md) —
> a 5-minute walkthrough from `git clone` to first plugin running in VS
> Code. The rest of this README is the reference docs.

---

## Quick Start (external plugin against a cloned xInsp2)

```bash
# 1. Clone xInsp2 once
git clone <xInsp2-url>  C:\dev\xInsp2

# 2. Set XINSP2_ROOT (do this once — env var, .bashrc, or .env)
set XINSP2_ROOT=C:\dev\xInsp2

# 3. Scaffold a new plugin in any folder you want
node %XINSP2_ROOT%\sdk\scaffold.mjs C:\dev\my_plugins\foo

# 4. Build it
cmake -S C:\dev\my_plugins\foo -B C:\dev\my_plugins\foo\build -A x64
cmake --build C:\dev\my_plugins\foo\build --config Release

# 5. Tell xInsp2 to load it (one of):
#    - VS Code setting:  xinsp2.extraPluginDirs = ["C:\\dev\\my_plugins"]
#    - CLI flag:         xinsp-backend.exe --plugins-dir=C:\dev\my_plugins
```

The plugin folder is yours — xInsp2 stays read-only. Edit, rebuild, the
host hot-reloads. Cert files, screenshots, instance data all live next
to your source. Upgrade by `git pull`-ing xInsp2 and rebuilding.

---

## What is a plugin?

A plugin is a DLL that exposes a C ABI the host calls on every frame.
The host handles:

- discovering the plugin (from its `plugin.json` manifest)
- creating instances (one plugin → many configured instances)
- hosting the config GUI (webview loaded from `ui/index.html`)
- wiring images + data between plugins via `xi::use("name")` (a sealed **pack** crosses each hop — see below)
- persisting state (instance config lives in `<project>/instances/<name>/instance.json`)

You write three things:

| File | Purpose |
|------|---------|
| `plugin.json`         | Manifest: name, factory, whether a GUI exists |
| `<name>.cpp`          | One class inheriting `xi::Plugin`; `XI_PLUGIN_IMPL(Class)` + `XI_PLUGIN_PACK_DOOR(Class)` for a data-plane plugin |
| `ui/index.html` (opt) | Webview HTML+JS that posts messages to/from the plugin |

That's it. Drop the folder into the host's `plugins/` folder (or any dir on
`xinsp2.extraPluginDirs`) and it is scanned on startup.

---

## Minimum viable plugin

```cpp
#include <xi/xi_abi.hpp>

class Hello : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // The xi.pack@1 door — the sole data plane since the v12 ABI cut.
    // Read entries from the input pack; write entries to the output pack.
    void process(xi::PackIn& in, xi::PackOut& out) override {
        std::string who(in.str("name").value_or("world"));
        out.str("greeting", "hello " + who);
    }
};

XI_PLUGIN_IMPL(Hello)
XI_PLUGIN_PACK_DOOR(Hello)   // publishes xi_plugin_get_interface("xi.pack",1)
```

That's a complete, working plugin. Inputs are optional (`in.str(...)`
returns `std::nullopt` when a key is absent); output is whatever you add
to `out`. Images and structured data share one sealed, keyed container —
the **pack** (`xi::PackIn` / `xi::PackOut`). See `examples/hello/`.

---

## Lifecycle

```
host scans plugin.json
         ↓
host calls xi_plugin_create(host_api, name)           → new Hello(...)
         ↓
host calls xi_plugin_set_def(stored_config_json)      → restore config if any
         ↓
┌──── for each inspection frame ──────────────────────────────────┐
│ host drives the xi.pack@1 door: process(PackIn&, PackOut&)      │
│ host calls xi_plugin_exchange(cmd_json, reply_buf) on UI clicks │
└─────────────────────────────────────────────────────────────────┘
         ↓ (on project save)
host calls xi_plugin_get_def() → persisted to instance.json
         ↓ (on shutdown)
host calls xi_plugin_destroy(inst)                    → delete instance
```

Everything is generated for you by `XI_PLUGIN_IMPL(Class)` — you only
override the virtuals you care about.

---

## The `xi::Plugin` interface

All virtuals have sensible defaults; override only what you need:

| Method | Purpose | Default |
|--------|---------|---------|
| `void process(xi::PackIn&, xi::PackOut&)` | Main work. Called per frame via the `xi.pack@1` door. | no-op (empty pack out) |
| `std::string exchange(const std::string& cmd)` | Handle UI button clicks / ad-hoc commands. | returns `"{}"` |
| `std::string get_def()` | Serialize config → JSON for persistence. | returns `"{}"` |
| `bool set_def(const std::string& json)` | Restore config from JSON. | returns `true` |

A data-plane plugin overrides `process(PackIn&, PackOut&)` **and** publishes
the door with `XI_PLUGIN_PACK_DOOR(Class)` (after `XI_PLUGIN_IMPL`) — that's
what the host probes to learn the plugin speaks packs.

A **source plugin** (camera / frame generator) instead runs its own capture
thread and calls the inherited `emit(...)` to push sealed packs into the
pipeline — see [Image sources and dispatch](#image-sources-and-dispatch). A
pure source needs no pack door.

### Per-instance storage

`get_def()` is for the small JSON config (a few hundred bytes) the host
serializes on project save. For anything larger — calibration images,
lookup tables, ML weights, captured reference frames — every instance
gets a dedicated folder on disk you can write whatever you want into:

```cpp
std::string folder = folder_path();          // available on xi::Plugin
auto path = std::filesystem::path(folder) / "ref.png";
std::ofstream f(path.string(), std::ios::binary);
// ... write your bytes ...
```

The path is `<project>/instances/<instance_name>/`. Properties:

- **Per instance, not per plugin.** Two instances of the same plugin
  each get their own folder
- **Created before your constructor runs**, so it's safe to write from
  `xi_plugin_create()` time
- **Never deleted by the host** — survives hot-reload, project
  open/close, host restart, instance recreate. Only the user can
  delete it
- **Inside the project folder** — copying or zipping the project
  carries all your instance data along
- `instance.json` (the host's serialization of `get_def()`) lives in
  the same folder, so small config + big files coexist naturally

Returns empty string if the plugin is running detached from a project.

---

## `xi::Json` cheatsheet

For parsing `exchange()` commands and building reply payloads (the JSON
control channel — distinct from the pack data plane). RAII — no manual
document free.

```cpp
#include <xi/xi_json.hpp>

// Parse
auto p = xi::Json::parse(cmd);
std::string command = p["command"].as_string();   // "" if missing
int n     = p["value"].as_int(0);                 // default if missing/wrong type
double t  = p["roi.threshold"].as_double(128.0);  // path access
bool flag = p["enabled"].as_bool(false);

// Iterate
p["points"].for_each([&](const char* idx, xi::Json v) {
    int x = v["x"].as_int();
});

// Build
auto reply = xi::Json::object()
    .set("ok", true)
    .set("count", 42)
    .set("name", "thing")
    .set("nested", xi::Json::object().set("k", "v"));

auto arr = xi::Json::array().push(1).push(2).push(3);
reply.set("nums", arr);

return reply.dump();         // compact
// or reply.dump_pretty();   // indented
```

**Compare to raw yyjson**: a typical exchange handler shrinks from
~12 lines (parse + null-checks + type checks + free) to 3 lines.

Reads on missing or wrong-typed fields return the supplied default
instead of crashing — no need to null-check at every step.

---

## `xi::PackIn` / `xi::PackOut` cheatsheet

The data plane is the **pack**: one sealed, keyed, typed container (canonical
msgpack — byte-identical in memory, on the WS wire, and on disk). Inside
`process()` you read `in` and write `out`. Typed reads return
`std::optional` (absence is explicit); adders chain.

```cpp
void process(xi::PackIn& in, xi::PackOut& out) override {
    // Read (optional-returning — nullopt on absent key / wrong type)
    int64_t n       = in.i64("count").value_or(0);
    bool    ok      = in.bool_or("pass", false);
    std::string lbl(in.str("label").value_or(""));

    // Read an image entry — zero-copy pixel span + dims. NEVER write through it.
    if (auto src = in.image("binary")) {
        const uint8_t* px = static_cast<const uint8_t*>(src->pixels);
        int w = src->width, h = src->height, c = src->channels;
        // ... wrap px in a cv::Mat, analyse ...
    }

    // Enumerate an unknown pack producer-agnostically
    in.for_each([&](std::string_view key, int tag) { /* ... */ });

    // Write typed entries
    out.i64("count", 5)
       .boolean("pass", true)
       .str("label", "ok");

    // Produce an output image in the host pool, then hand it over by
    // refcount (zero-copy — no heap→pool memcpy across the ABI)
    xi::Image dst = pool_image(w, h, 1);
    // ... write dst.write() ...
    out.adopt_image("overlay", dst.width, dst.height, dst.channels, dst.pool_handle());

    // Nested trees are msgpack's job — one entry (xi::mp::Writer),
    // read back with xi::mp::Reader
    // out.mp("items", writer.bytes().data(), writer.bytes().size());

    // Fail-loud: a missing required input is a normal sealed pack stamped
    // "$fault", never a silent default (the host short-circuits it downstream)
    if (!in.image("binary")) { out.fault("missing_input", "binary"); return; }
}
```

Producer identity (`$src`) and the hop chain (`$prov`) are stamped
automatically on every non-empty door output — see
[`../docs/internals/pack-plane.md`](../docs/internals/pack-plane.md) and
`xi/xi_pack_contract.hpp` for the reserved `$`-key contract.

---

## Image sources and dispatch

If your plugin is a **camera / image source** — something that pushes
frames into the pipeline rather than processing input — it emits a sealed
**pack** and the host dispatches the inspection script once per emit.
There is one dispatch verb: the inherited `emit(...)`.

### Emitting frames: `new_pack()` / `emit()`

Build a pack (one or more image entries, plus optional metadata as ordinary
keyed entries) and hand it to the host. The host stamps it with a **128-bit
trigger id**, dispatches the inspection exactly once, and the script reads the
frames back via `xi::current_trigger()`.

```cpp
#include <xi/xi_abi.hpp>   // xi::Plugin, xi::PackOut, xi::Image, pool_image()/new_pack()/emit()

void run_loop() {
    // Paint straight into a fresh host-pool slot, so the pack can adopt it
    // by refcount (no heap→pool copy).
    xi::Image img = pool_image(W, H, channels);
    // ... write pixels into img.write() ...

    xi::PackOut f = new_pack();               // starts a host-side builder
    f.i64("seq", seq);
    f.adopt_image("frame", W, H, channels, img.pool_handle());
    emit(std::move(f));                        // seals + dispatches, drops our ref
}
```

`emit()` is the `xi::Plugin` member that seals the pack, dispatches it, and
drops our ref — one call owns the whole `builder_seal` / `emit_pack` /
`release` refcount dance. Its full signature (defaults shown):

```cpp
void xi::Plugin::emit(xi::PackOut&& out,
                      xi_trigger_id id = XI_TRIGGER_NULL,  // null → host mints one
                      int64_t       ts = 0);               // 0 → host clock
```

`id == XI_TRIGGER_NULL` asks the host for a fresh id (its hex is
`current_trigger().id_string()`, used by the buffer_replay plugin to
replay a run). Routing/context metadata rides as ordinary pack entries
alongside the image: `f.i64("recipe", 7)`.

See `sdk/examples/trigger_source/` for a complete runnable source plugin.

### Reading a pack from a script

Scripts read the current event via `xi::current_trigger()`; the payload is
the trigger's pack (`t.pack()`, a `ScriptPack`):

```cpp
#include <xi/xi_use.hpp>
XI_INSPECT_ENTRY(t, frame) {         // t = the trigger, passed in explicitly
    (void)frame;
    if (!t.is_active()) return;
    auto id = t.id_string();
    if (auto f = t.pack()) {                 // empty if the event carries no pack
        auto img = f.get_image("frame");     // key matches what the source emitted
        int64_t seq = f.get_i64("seq").value_or(-1);   // metadata = ordinary entries
        // Surface results by pushing a pack to the expose sink:
        xi::ScriptPackBuilder b;
        b.add_str("$channel", "main");
        if (img) b.add_image("frame", *img);
        b.add_str("id", id);
        xi::use("expose").push(b.seal());
    }
}
```

`f.get_image(key)` fetches an image entry by the key the source used;
`f.get_i64/get_str/...` read metadata entries; `f.for_each(...)` enumerates
an unknown pack. The frame is stored under whatever key the source emitted
(`"frame"` above), so the read works identically whether the frame arrived
from a live source, a `cmd:run --frame` inject, or a replay.

### Correlating multiple sources

Bus correlation policies were removed — there is no `set_trigger_policy`.
To fire one inspection from several sources "at the same event" (e.g. a
hardware-synced stereo pair), write a **gathering plugin**: it subscribes
to the source instances, combines their latest frames into ONE pack
(distinct keys like `"left"` / `"right"`, optionally sharing a trigger id),
and `emit()`s that single combined pack. See `examples/stereo_sync/` for a
paired-cameras reference.

### Recording and replay

Replay is a plugin, not a core feature. The **buffer_replay** plugin
captures emitted packs and re-emits them through the same `emit()` path,
so the whole pipeline sees them identically to a live run — and because a
sealed pack is immutable, the re-emit is **byte-lossless**. Good for
regression tests and off-line tuning. See `examples/buffer_replay_demo/`.

---

## Writing a config UI

`ui/index.html` is a plain HTML file with one script that calls
`vscode.postMessage()` to talk to the plugin.

```html
<script>
const vscode = acquireVsCodeApi();

// Send a command to C++ side
function apply() {
    vscode.postMessage({
        type: 'exchange',
        cmd: { command: 'set_threshold', value: 128 }
    });
}

// Receive state from C++ side (pushed after every exchange / on open)
window.addEventListener('message', e => {
    if (e.data.type === 'status') updateUI(e.data);
});

// Request initial state when the UI opens
vscode.postMessage({ type: 'exchange', cmd: { command: 'get_status' } });
</script>
```

Your C++ `exchange(cmd)` parses the JSON, applies the command, and
returns the new state — the host automatically forwards that back to
the webview as `{type: 'status', ...your_return_json}`.

See `examples/counter/ui/index.html` for a minimal working UI.

---

## `plugin.json` manifest

```json
{
  "name": "my_plugin",
  "description": "What it does (shown in the + picker)",
  "dll": "my_plugin.dll",
  "factory": "xi_plugin_create",
  "has_ui": true
}
```

- `name` — **required**; must be unique across all plugins on disk
- `description` — optional; shown in the instance-creation picker
- `dll` — optional; defaults to `<name>.dll`, relative to the plugin folder
- `factory` — optional; defaults to `xi_plugin_create` (what `XI_PLUGIN_IMPL` emits).
  Override only if you export a non-standard symbol name
- `has_ui` — optional; `true` means the host expects `ui/index.html` next
  to the DLL. Any other value (or absence) means "no UI"

---

## Building

Each plugin is an independent shared library. Minimum CMakeLists:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_plugin)

set(CMAKE_CXX_STANDARD 20)
set(XINSP2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_library(my_plugin SHARED my_plugin.cpp ${XINSP2_ROOT}/backend/vendor/yyjson/yyjson.c)
target_include_directories(my_plugin PRIVATE
    ${XINSP2_ROOT}/backend/include
    ${XINSP2_ROOT}/backend/vendor/yyjson)
```

(Or just use the `xinsp2_add_plugin(...)` helper from
`sdk/cmake/xinsp2_plugin.cmake`, which wires the include dirs + yyjson in
for you — that's what the examples use.) Point `XINSP2_ROOT` at the xInsp2
checkout. After `cmake --build . --config Release`,
copy the `.dll` + `plugin.json` + `ui/` into `<xInsp2>/plugins/<name>/`
(or just build directly into that folder — see the template).

---

## What's in this SDK

```
sdk/
├── README.md           ← you are here
├── scaffold.mjs        ← CLI: scaffold a new plugin from a template
├── scaffold/render.mjs ← shared template renderer (used by extension too)
├── templates/          ← single source of truth for both VS Code + CLI
│   │                     ONE xi::Plugin skeleton, three layers (see below)
│   ├── easy/           ← Layer 0: process() only — the bare skeleton
│   ├── medium/         ← Layer 1: + config/params (xi::Json) + status() + image op + UI
│   ├── expert/         ← Layer 2: + source worker (xi::spawn_worker) emitting via emit() + UI
│   └── _shared/        ← reusable HTML snippets (image_viewer_widget.html)
└── qa/
    ├── hello/          ← 1 file, no state, no UI — the "hello world"
    ├── counter/        ← persistent state + minimal UI (xi::Json)
    ├── invert/         ← image-in → image-out
    ├── histogram/      ← image analysis with rich JSON output
    └── trigger_source/ ← image source using new_pack()/emit() (push frames)
```

**One spine, three layers.** The `easy` / `medium` / `expert` templates are the
same `xi::Plugin` skeleton with progressively more turned on — never three
different architectures. `easy` overrides `process()` only; `medium` adds
config/params + `status()` + a real image op; `expert` adds a background source
worker (`xi::spawn_worker`) that pushes frames via `emit()`. Because every tier
inherits `xi::Plugin`, every tier gets `pool_image()`, `status()`, `compress()`,
`emit()`, and the capability wrappers for free — the tier you pick decides how
much is *enabled*, not which base class or style you learn.

Two ways to start a new plugin:

1. **In-project** (fastest iteration):
   In VS Code, run **xInsp2: New Project Plugin…** — picks a template,
   asks for a name, drops files into `<project>/plugins/<name>/`, backend
   auto-compiles on save with hot-reload + state preservation. Export it
   later via **xInsp2: Export Project Plugin…** to produce a standalone
   deployable.

2. **Standalone** (for distribution):
   ```sh
   node sdk/scaffold.mjs <output-dir> --name foo --template medium
   cmake -S <output-dir> -B <output-dir>/build -A x64
   cmake --build <output-dir>/build --config Release
   ```
   Adds CMakeLists + README so it builds on its own. Same templates as
   the in-project path; output is byte-identical for shared files.

The `qa/` folder shows what to look at for specific patterns
(state, image ops, trigger source). Read them in order — each adds one
capability.

---

## Testing your plugin

Plugins are **trusted** — the host loads your DLL straight through after a
one-time ABI-version check (no certification gate, no `cert.json`). Testing
is therefore **optional and owned by you**: nothing blocks a plugin from
loading, so write the tests that give *you* confidence.

### Native C++ tests (`xi_test.hpp`)

The SDK ships a tiny test framework — `XI_TEST` / `XI_EXPECT` /
`xi::test::run_all()` — for exercising your plugin's logic directly. A
useful starting set probes the surface the host actually calls:
create/destroy, `get_def → set_def` round-trip, `exchange("{}")` returns
valid JSON, `process()` on empty input, and a few concurrent `process()`
calls (the host may dispatch your plugin from parallel worker lanes).

```cpp
#include <xi/xi_test.hpp>   // XI_TEST, XI_EXPECT, xi::test::run_all

XI_TEST(get_set_def_roundtrip) {
    MyPlugin p("inst0");
    auto a = p.get_def();
    XI_EXPECT(p.set_def(a));
    XI_EXPECT_EQ(p.get_def(), a);   // reports file:line + the failing expression
}

XI_TEST(my_custom_behavior) {
    // Your plugin-specific assertions here
    XI_EXPECT(some_observation == expected_value);
}

int main() { auto r = xi::test::run_all(); for (auto& t : r) if (!t.passed) return 1; return 0; }
```

Build it as an ordinary executable linking your plugin source plus the SDK
include dir (`${XINSP2_ROOT}/backend/include`); `add_test(...)` wires it
into ctest. Use `XI_EXPECT(cond)` / `XI_EXPECT_EQ(a, b)`; keep one instance
per test (construct → probe → destroy in the test body); clean up any files
/ env vars a test touches so runs stay independent.

---

## UI / E2E tests

Native C++ tests cover correctness of the plugin's logic. **UI tests**
cover the rest of the user journey — the webview, the commands the
extension wires up, the round-trip through the live host. Both live in
the plugin folder and both are owned by you.

### Layout

```
my_plugin/
├── plugin.json
├── my_plugin.cpp
├── ui/index.html
├── CMakeLists.txt
└── tests/
    ├── test_native.cpp     ← C++ xi_test tests (your test.exe)
    ├── test_ui.cjs         ← UI/E2E test (one CJS module)
    └── screenshots/        ← created automatically by h.shot(...)
```

### `tests/test_ui.cjs` shape

One module exporting `run(h)`. The launcher hands you `h`, a helpers
object that drives VS Code commands and the live webview.

```js
module.exports = {
    async run(h) {
        const projDir = h.tmp();
        await h.createProject(projDir, 'demo');
        await h.addInstance('inst0', 'my_plugin');
        await h.openUI('inst0', 'my_plugin');

        // Drive the actual DOM
        h.setInput('inst0', '#threshold', 128);
        h.click('inst0', '.btn-apply');
        await h.sleep(300);
        h.shot('after_apply');

        // Round-trip status (sets h.lastStatus)
        await h.getStatus('inst0');
        h.expectEq(h.lastStatus.threshold, 128);
    },
};
```

### Helpers (`h`) API

| Call | What |
|------|------|
| `h.createProject(folder, name)` | runs `xinsp2.createProject` |
| `h.addInstance(name, plugin)`   | runs `xinsp2.createInstance` |
| `h.openUI(name, plugin)`        | opens the webview, waits for it to mount |
| `h.click(inst, selector)`       | dispatches a real `click` inside the webview DOM |
| `h.setInput(inst, sel, value)`  | sets `value`, fires `input` + `change` events |
| `h.sendCmd(inst, cmd)`          | exchange round-trip; updates `h.lastStatus` |
| `h.getStatus(inst)`             | shorthand for `sendCmd(inst, {command:'get_status'})` |
| `h.run()`                       | runs `xinsp2.run` (one inspection cycle) |
| `h.shot(label)`                 | full-screen PNG into `tests/screenshots/` |
| `h.expect(cond, msg)`           | soft assertion — recorded, doesn't throw |
| `h.expectEq(a, b, msg)`         | structural equality, JSON.stringify-compared |
| `h.sleep(ms)`                   | promise-based sleep |
| `h.tmp()`                       | unique per-test temp dir |
| `h.lastStatus`                  | last successful exchange's parsed payload |
| `h.failures`, `h.passes`        | arrays of recorded assertion outcomes |

UI tests don't have to throw to fail — they accumulate. The runner
reports the count and exit-codes non-zero if any failures recorded.

### Two ways to run

**CLI** (cold session — clean every time, what CI uses):

```
node <xinsp2>/sdk/testing/run_ui_test.mjs <plugin-folder>
```

Auto-detects `XINSP2_ROOT` by walking parent directories looking for
`backend/` and `vscode-extension/`. Set the env var explicitly to skip
auto-detection:

```
set XINSP2_ROOT=C:\path\to\xInsp2
```

**VS Code command** (warm session — saves the ~12s cold-start when
iterating on the test itself):

```
Cmd Palette → "xInsp2: Run Plugin UI Tests"
```

Pick the plugin folder when prompted. Both run paths load the same
`test_ui.cjs` and pass the same `h`. Set `xinsp2.sdkPath` if your SDK
isn't at `<extension>/../sdk`.

### Native vs UI tests

Native C++ tests are machine-portable — pure logic, no environment. UI
tests can depend on dev-only data (reference images, calibration files)
and on a real VS Code instance with screen access — those signals don't
transfer between machines, so keep the two suites separate.

---

## Tips

- **Debug prints**: `std::fprintf(stderr, "[myplug] %d\n", x)` shows up in
  the backend's output window
- **Hot reload**: the host reloads your DLL on rebuild; instance state is
  preserved (backed by `get_def`/`set_def`), so you can iterate without
  restarting the whole host
- **Don't block**: `process()` runs on a dispatch worker thread. If you
  need to wait (hardware, network), do it on your own worker thread (a
  source plugin's `xi::spawn_worker` capture thread that calls `emit()`)
- **Sharing images is free**: images live in the host pool and cross the
  pack by refcount (`adopt_image`) — handing a frame to the output pack
  copies no pixel bytes
- **Raw host handles** (rare): if you need RAII over an
  `xi_image_handle` — e.g. passing a frame between plugins without
  decoding it — use the `HostImage` factories from `xi_abi.hpp`:
  `HostImage::from_handle(host, h)` takes ownership of an existing
  refcount-1 handle without addref; `HostImage::share_handle(host, h)`
  addrefs for an independent view. The `(host, handle)` constructor is
  deliberately private because mixing it with `image_create()` is a
  refcount trap
- **JSON**: prefer `xi::Json` (RAII, path access, defaults) for
  exchange commands and replies. Raw `yyjson` (via `xi_json.hpp`'s
  `yyjson.h`) is available for performance-critical paths
