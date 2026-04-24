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
- wiring images + records between plugins via `xi::use("name")`
- persisting state (instance config lives in `<project>/instances/<name>/instance.json`)

You write three things:

| File | Purpose |
|------|---------|
| `plugin.json`         | Manifest: name, factory, whether a GUI exists |
| `<name>.cpp`          | One class inheriting `xi::Plugin`, one macro `XI_PLUGIN_IMPL(Class)` |
| `ui/index.html` (opt) | Webview HTML+JS that posts messages to/from the plugin |

That's it. Drop the folder into `plugins/` and the host scans it on startup.

---

## Minimum viable plugin

```cpp
#include <xi/xi_abi.hpp>

class Hello : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        std::string who = input["name"].as_string("world");
        return xi::Record().set("greeting", "hello " + who);
    }
};

XI_PLUGIN_IMPL(Hello)
```

That's a complete, working plugin. Inputs are optional; output is
whatever you return. Images and structured data share one bag
(`xi::Record`).

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
│ host calls xi_plugin_process(input_record, out_record)          │
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
| `xi::Record process(const xi::Record&)` | Main work. Called per frame. | returns `{}` |
| `std::string exchange(const std::string& cmd)` | Handle UI button clicks / ad-hoc commands. | returns `"{}"` |
| `std::string get_def()` | Serialize config → JSON for persistence. | returns `"{}"` |
| `bool set_def(const std::string& json)` | Restore config from JSON. | returns `true` |
| `void start()` / `void stop()` | For streaming sources (cameras). | no-op |

`xi::ImageSource` is a subclass for cameras — adds `grab()` /
`grab_wait()` and a built-in frame queue with backpressure.

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

For parsing `exchange()` commands and building reply payloads. RAII —
no manual `cJSON_Delete`. Same path syntax as `xi::Record`.

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

**Compare to raw cJSON**: a typical exchange handler shrinks from
~12 lines (parse + null-checks + type checks + delete) to 3 lines.

Reads on missing or wrong-typed fields return the supplied default
instead of crashing — no need to null-check at every step.

---

## `xi::Record` cheatsheet

```cpp
// Build
xi::Record r;
r.set("count", 5)
 .set("pass", true)
 .set("label", "ok")
 .image("binary", img)
 .image("overlay", rgb);

// Nested objects
r.set("roi", xi::Record().set("x", 10).set("y", 20));

// Read (with default)
int n     = r["count"].as_int(0);
bool ok   = r["pass"].as_bool(false);
std::string lbl = r["label"].as_string("");

// Path access
int x     = r["roi.x"].as_int();
int first = r["points[0].value"].as_int();

// Images
const xi::Image& img = r.get_image("binary");
for (auto& [key, img] : r.images()) { /* iterate */ }
```

---

## Image sources and the trigger bus

If your plugin is a **camera / image source** — something that pushes
frames into the pipeline rather than processing input — you have two
choices, in increasing order of capability:

### Old style: `xi::ImageSource` subclass

Inherit `xi::ImageSource`, implement `grab()` / `grab_wait()`. The host
wraps each push into a single-frame trigger automatically (via
`attach_trigger_bridge`). Works for legacy plugins unchanged. Fine for
a single free-running camera, not enough for correlated multi-source.

### New style: `host->emit_trigger(...)`

Call `emit_trigger` directly to publish one or more frames under a
**128-bit trigger id**. The host's bus correlates emissions that share
a tid and dispatches the inspection exactly once per complete event.

```cpp
// host_api signature
void emit_trigger(
    const char*            source_name,   // this instance's name()
    xi_trigger_id          tid,           // shared id, or XI_TRIGGER_NULL
    int64_t                timestamp_us,  // 0 → host clock
    const xi_record_image* images,        // N key → handle entries
    int32_t                image_count);
```

Minimum single-source usage:

```cpp
xi_image_handle h = host_->image_create(W, H, ch);
// ... write pixels into host_->image_data(h) ...
xi_record_image entry = { "frame", h };
host_->emit_trigger(name().c_str(), XI_TRIGGER_NULL, /*ts=*/0, &entry, 1);
host_->image_release(h);  // bus addref'd internally; drop our refcount
```

See `sdk/examples/trigger_source/` for a complete runnable plugin and
`plugins/synced_stereo/` in the xInsp2 tree for a paired-cameras
reference.

### Trigger policies

Projects pick one of three bus policies (`cmd: set_trigger_policy`,
persisted in `project.json`):

| Policy             | Dispatch rule                                               |
|--------------------|-------------------------------------------------------------|
| `any`              | Fire on every emit (default, back-compat)                   |
| `all_required`     | Fire only when every source in the required list has emitted for that tid; drop incomplete tids after `window_ms` |
| `leader_followers` | Fire on the leader emit; attach followers' most recent frames (best-effort) |

### Reading a trigger from a script

Scripts read the current event via `xi::current_trigger()`:

```cpp
#include <xi/xi_use.hpp>
void xi_inspect_entry(int frame) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;
    VAR(tid,   t.id_string());
    VAR(left,  t.image("cam_left"));         // single-frame source
    VAR(right, t.image("synced0/right"));    // multi-frame source
}
```

Multi-frame sources use `<source>/<image_name>` keys; single-frame
sources use just the source name. `t.sources()` lists every source that
contributed to this event.

### Recording and replay

Any live project can be recorded: `recording_start` installs an
observer on the bus that serialises every TriggerEvent to disk (manifest
+ `.raw` pixels). `recording_replay` pushes those events back through
`emit_trigger` so the whole pipeline — including sinks and observers —
sees them identically to the live run. Good for regression tests and
off-line tuning.

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

- `name` must be unique across all plugins on disk
- `dll` is relative to the plugin folder
- `has_ui: true` → host expects `ui/index.html`
- `factory` should always be `xi_plugin_create` (provided by `XI_PLUGIN_IMPL`)

---

## Building

Each plugin is an independent shared library. Minimum CMakeLists:

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_plugin)

set(CMAKE_CXX_STANDARD 20)
set(XINSP2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)

add_library(my_plugin SHARED my_plugin.cpp ${XINSP2_ROOT}/backend/vendor/cJSON.c)
target_include_directories(my_plugin PRIVATE
    ${XINSP2_ROOT}/backend/include
    ${XINSP2_ROOT}/backend/vendor)
```

Point `XINSP2_ROOT` at the xInsp2 checkout. After `cmake --build . --config Release`,
copy the `.dll` + `plugin.json` + `ui/` into `<xInsp2>/plugins/<name>/`
(or just build directly into that folder — see the template).

---

## What's in this SDK

```
sdk/
├── README.md           ← you are here
├── template/           ← copy this folder, rename, edit
└── examples/
    ├── hello/          ← 1 file, no state, no UI — the "hello world"
    ├── counter/        ← persistent state + minimal UI (xi::Json)
    ├── invert/         ← image-in → image-out
    ├── histogram/      ← image analysis with rich JSON output
    └── trigger_source/ ← image source using host->emit_trigger (bus)
```

Read them in order — each one adds one new capability.

---

## Testing your plugin

Every plugin you write has to pass a fixed set of **baseline tests** before
the host will load it. These probe the C ABI surface — create/destroy,
JSON round-trip, concurrent calls, empty input — the classes of bug that
would otherwise take down the whole host on first use.

### How certification works

```
first scan of plugins/my_plugin/
         ↓
host looks for plugins/my_plugin/cert.json
         ↓
    ┌────┴───────┬──────────────────────────┐
  missing     stale                    valid
  ↓             ↓                        ↓
  run baseline tests              skip — plugin loads
  ↓                                      ↓
  pass → write cert.json           ready to instantiate
  fail → don't load, log the
         failing tests to stderr
```

A cert is stale if *any* of:

- DLL size changed (rebuild)
- DLL mtime changed (rebuild)
- `baseline_version` in the cert is lower than the current one (the host
  added a new baseline test)

**Certs are gradually tightened.** The baseline set is expected to grow —
each added test bumps `BASELINE_VERSION` and invalidates every existing
cert, forcing a re-run on next load. This is the mechanism for rolling
out new correctness requirements across all plugins.

### Running tests yourself

Write a test binary that links the baseline header and your plugin. See
`examples/counter/tests/test_counter.cpp` for a working template:

```cpp
#include <xi/xi_test.hpp>       // XI_TEST, XI_EXPECT, run_all
#include <xi/xi_baseline.hpp>   // the baseline tests
#include <xi/xi_cert.hpp>       // cert::certify() writes cert.json on pass

XI_TEST(baseline_all_pass) {
    auto summary = xi::baseline::run_all(syms, &host);
    XI_EXPECT(summary.all_passed);
    xi::cert::certify(plugin_folder, dll_path, "my_plugin", syms, &host);
}

XI_TEST(my_custom_behavior) {
    // Your plugin-specific assertions here
    XI_EXPECT(some_observation == expected_value);
}

int main() { auto r = xi::test::run_all(); for (auto& t : r) if (!t.passed) return 1; return 0; }
```

Build via CMake (see `counter/CMakeLists.txt`):

```cmake
add_executable(my_plugin_test tests/test_my_plugin.cpp
    ${XINSP2_ROOT}/backend/vendor/cJSON.c)
target_include_directories(my_plugin_test PRIVATE
    ${XINSP2_ROOT}/backend/include
    ${XINSP2_ROOT}/backend/vendor)
target_compile_definitions(my_plugin_test PRIVATE
    MY_PLUGIN_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/my_plugin.dll")
add_test(NAME my_plugin_test COMMAND my_plugin_test)
```

`cmake --build . --config Release --target my_plugin_test && ./my_plugin_test`
runs both the baseline and your custom tests, and (on pass) writes
`cert.json` — so the host won't re-run the baselines next time it loads
your plugin.

### What the baseline covers today

| Test | Checks |
|------|--------|
| `factory_create_destroy`     | `create()` returns non-null; `destroy()` doesn't crash |
| `get_def_returns_valid_json` | `get_def()` output parses as JSON |
| `get_set_def_roundtrip`      | `get_def → set_def → get_def` is stable |
| `exchange_valid_json`        | `exchange("{}")` returns valid JSON |
| `process_empty_input`        | `process(empty Record)` doesn't crash |
| `concurrent_process`         | 4 threads × 50 `process()` calls, no races |
| `concurrent_mixed`           | Mixed `process()` + `exchange()` across threads |
| `many_create_destroy`        | 20 rapid create/destroy cycles, no leaks-that-crash |

Total runtime: typically under 50ms per plugin. Called on first load;
cached via cert thereafter.

### Writing tests that help you

- Use `XI_EXPECT(cond)` / `XI_EXPECT_EQ(a, b)` — they report file:line + the
  failing expression
- One instance per test is the easiest pattern — construct, probe,
  destroy inside the test body
- For flows that touch shared state (files, environment variables) clean
  up after yourself or each test run can leave artifacts that break the
  next one
- Custom tests don't invalidate certs — only baseline changes do

---

## UI / E2E tests

Native C++ tests cover correctness of the plugin's logic. **UI tests**
cover the rest of the user journey — the webview, the commands the
extension wires up, the round-trip through the live host. Both live in
the plugin folder, both are owned by you, neither runs on the host's
critical path (no UI test is part of cert).

### Layout

```
my_plugin/
├── plugin.json
├── my_plugin.cpp
├── ui/index.html
├── CMakeLists.txt
└── tests/
    ├── test_native.cpp     ← C++ baseline + custom (your test.exe)
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

### Why UI tests aren't part of cert

`cert.json` asserts the DLL won't take down the host (C-ABI safety
gate) — it should be machine-portable. UI tests can depend on dev-only
data (reference images, calibration files) and on a real VS Code
instance with screen access — those signals don't transfer between
machines, so they stay separate from cert.

---

## Tips

- **Debug prints**: `std::fprintf(stderr, "[myplug] %d\n", x)` shows up in
  the backend's output window
- **Hot reload**: the host reloads your DLL on rebuild; instance state is
  preserved (backed by `get_def`/`set_def`), so you can iterate without
  restarting the whole host
- **Don't block**: `process()` runs on the inspection thread. If you need
  to wait (hardware, network), subclass `xi::ImageSource` or spawn your
  own worker
- **Sharing images is free**: `xi::Image` uses a `shared_ptr` to pixels —
  copying a Record does not copy image bytes
- **JSON**: prefer `xi::Json` (RAII, path access, defaults) for
  exchange commands and replies. Raw cJSON is still re-exported via
  `xi_abi.hpp` for performance-critical paths or legacy plugins
