# xInsp2 Framework Reference

Complete technical reference for the xInsp2 machine vision inspection framework.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [User Script API](#2-user-script-api)
3. [Record — The Universal Data Container](#3-record)
4. [Image Type](#4-image-type)
5. [Operator Library (xi_ops)](#5-operator-library)
6. [Parallelism (xi_async)](#6-parallelism)
7. [Parameters (xi_param)](#7-parameters)
8. [Plugin System](#8-plugin-system)
9. [C ABI Reference](#9-c-abi-reference)
10. [Image Pool & Handles](#10-image-pool)
11. [WebSocket Protocol](#11-websocket-protocol)
12. [Project Structure](#12-project-structure)
13. [VS Code Extension](#13-vs-code-extension)
14. [Build System](#14-build-system)
15. [File Reference](#15-file-reference)

---

## 1. Architecture Overview

```
┌─────────────────────────────────┐     ┌──────────────────────────────┐
│ xinsp-backend.exe               │     │ VS Code Extension            │
│                                 │     │                              │
│  Plugin Manager                 │     │  wsClient.ts                 │
│    ├─ mock_camera.dll           │ WS  │  instanceTree.ts             │
│    ├─ blob_analysis.dll         │◄───►│  viewerProvider.ts           │
│    └─ data_output.dll           │JSON │  instanceCodeLens.ts         │
│                                 │+JPEG│  extension.ts                │
│  Script Compiler (MSVC cl.exe)  │     │                              │
│    └─ user's inspection.dll     │     │  Plugin UI webviews          │
│                                 │     │    ├─ mock_camera/ui/        │
│  Image Pool (refcounted)        │     │    └─ blob_analysis/ui/      │
│  WebSocket Server               │     │                              │
└─────────────────────────────────┘     └──────────────────────────────┘
```

**Three processes:**
- **Backend** (`xinsp-backend.exe`) — loads plugins, compiles user scripts, runs inspections, serves WS
- **VS Code** — editor with the xInsp2 extension for UI, viewer, and project management
- **Plugin DLLs** — loaded at runtime via the C ABI, called through function pointers

**Data flow:**
```
User .cpp → compile → .dll → inspect() → VAR() → ValueStore
    → snapshot thunks → JSON + JPEG → WebSocket → VS Code viewer
```

---

## 2. User Script API

A user script is a single `.cpp` file compiled at runtime into a DLL.

### Minimal script

```cpp
#include <xi/xi.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    VAR(hello, 42);
}
```

### Full-featured script

```cpp
#include <xi/xi.hpp>
#include <xi/xi_ops.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_plugin_handle.hpp>
#include <xi/xi_source.hpp>

using namespace xi::ops;

// Tunable parameters (show in VS Code sidebar)
xi::Param<int>    thresh {"threshold", 128, {0, 255}};
xi::Param<double> sigma  {"sigma",     2.0, {0.1, 10.0}};

// Plugin handles (loaded via C ABI at DLL load time)
xi::PluginHandle blobs{"detector0", "blob_analysis"};

// Camera source (runs its own acquisition thread)
static xi::TestImageSource cam("cam0", 640, 480, 10);
struct AutoStart {
    AutoStart()  { cam.start(); }
    ~AutoStart() { cam.stop(); }
} static g_auto;

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto img = cam.grab_wait(500);
    if (img.empty()) return;

    VAR(input, img);
    VAR(gray, toGray(img));
    VAR(blurred, gaussian(gray, (int)sigma));

    // Call plugin via C ABI
    auto result = blobs.process(xi::Record()
        .image("gray", gray)
        .set("threshold", (int)thresh)
        .set("min_area", 50));

    VAR(detection, result);
    VAR(pass, result["blob_count"].as_int() <= 3);
}
```

### Macros available in scripts

| Macro | Purpose | Example |
|-------|---------|---------|
| `VAR(name, expr)` | Track variable for viewer display | `VAR(gray, toGray(img));` |
| `VAR_RAW(name, expr)` | Track with uncompressed BMP preview | `VAR_RAW(binary, threshold(gray, t));` |
| `XI_SCRIPT_EXPORT` | Portable DLL export decorator | `XI_SCRIPT_EXPORT void xi_inspect_entry(int)` |
| `ASYNC_WRAP(fn)` | Generate `async_fn(args...)` wrapper | `ASYNC_WRAP(myFilter)` |

### Script lifecycle

```
cmd: compile_and_load
  → vcvars64.bat + cl.exe /LD → inspection.dll
  → LoadLibrary → resolve xi_inspect_entry
  → global constructors run (Param, PluginHandle register)

cmd: run
  → xi_script_reset() clears ValueStore
  → xi_inspect_entry(frame) called
  → xi_script_snapshot_vars() reads tracked values
  → JSON + JPEG sent to viewer
```

---

## 3. Record

`xi::Record` is the universal data container. Every plugin input/output and every `VAR()` of a complex result uses Record.

### Header

```cpp
#include <xi/xi_record.hpp>
```

### Building

```cpp
xi::Record r;
r.set("count", 5);                          // int
r.set("score", 0.95);                       // double
r.set("pass", true);                        // bool
r.set("label", "ok");                       // string
r.image("input", img);                      // named image
r.image("edges", edge_img);                 // multiple images

// Nested record
r.set("roi", xi::Record()
    .set("x", 100).set("y", 50)
    .set("width", 200).set("height", 150));

// Array of records
for (auto& match : matches) {
    r.push("items", xi::Record()
        .image("roi", match.roi)
        .set("area", match.area)
        .set("pass", match.pass));
}
```

### Reading (safe chaining with defaults)

```cpp
// Style 1: chained operator[]
int x = r["roi"]["x"].as_int(0);
double score = r["items"][0]["score"].as_double();
bool ok = r["pass"].as_bool(false);
std::string s = r["label"].as_string("unknown");

// Style 2: path expression
int x = r["roi.x"].as_int(0);
double s = r["items[0].score"].as_double();

// Style 3: explicit .at()
int x = r.at("roi.x").as_int(0);

// Style 4: direct getters
int x = r.get_int("count", 0);
auto sub = r.get_record("roi");
int n = r.get_array_size("items");

// Check existence
if (r["mask"].exists()) { ... }
if (r["items"].is_array()) { ... }
int n = r["items"].size();

// Image access
xi::Image img = r.get_image("input");
bool has = r.has_image("mask");

// All safe — missing keys return defaults, never crash
r["a"]["b"]["c"]["d"].as_int(42);  // → 42
```

### Serialization

```cpp
std::string json = r.data_json();         // compact JSON
std::string pretty = r.data_json_pretty(); // formatted JSON
std::string keys = r.image_keys_json();   // ["input","edges"]
```

### Internals

Record uses **cJSON** internally. Images use `xi::Image` with `shared_ptr<vector<uint8_t>>` — copying a Record shares pixel buffers (zero-copy).

---

## 4. Image Type

### Header

```cpp
#include <xi/xi_image.hpp>
```

### API

```cpp
xi::Image img(640, 480, 3);               // allocate w×h×channels
xi::Image img(w, h, c, raw_ptr);          // copy from raw pointer
xi::Image copy = img;                      // shared_ptr copy (zero-copy)

bool       img.empty();
size_t     img.size();                     // total bytes
uint8_t*   img.data();                     // pixel pointer
int        img.width, img.height, img.channels;
int        img.stride();                   // width * channels
```

Layout: row-major, interleaved channels, uint8 pixels. 1 (gray), 3 (RGB), or 4 (RGBA).

---

## 5. Operator Library

### Header

```cpp
#include <xi/xi_ops.hpp>
using namespace xi::ops;
```

### Available operators

| Function | Input | Output | Description |
|----------|-------|--------|-------------|
| `toGray(img)` | RGB/RGBA | Gray | BT.601 luminance conversion |
| `threshold(gray, t, maxval)` | Gray | Gray | Binary threshold |
| `gaussian(gray, radius)` | Gray | Gray | 3-pass box blur approximation |
| `boxBlur(gray, radius)` | Gray | Gray | Single-pass box blur |
| `sobel(gray)` | Gray | Gray | Edge magnitude (3×3 Sobel) |
| `invert(img)` | Any | Same | 255 - pixel |
| `erode(gray, radius)` | Gray | Gray | Minimum filter |
| `dilate(gray, radius)` | Gray | Gray | Maximum filter |
| `stats(gray)` | Gray | `ImageStats` | Mean, stddev, min, max |
| `countWhiteBlobs(binary)` | Gray | `int` | Flood-fill connected components |

### Async variants

Every operator has an `async_` version for parallel execution:

```cpp
auto p1 = async_sobel(gray);
auto p2 = async_threshold(gray, 128);
xi::Image edges  = p1;    // await
xi::Image binary = p2;    // await
```

### When to use xi_ops vs plugins

- **xi_ops**: lightweight, inline, zero overhead. For fast per-pixel ops.
- **Plugins**: configurable, hot-swappable, UI-equipped. For complex algorithms.

---

## 6. Parallelism

### Header

```cpp
#include <xi/xi_async.hpp>
```

### API

```cpp
// Spawn a task
auto future = xi::async(function, arg1, arg2, ...);

// Implicit await via type conversion
ResultType result = future;

// Explicit await
auto result = future.get();

// Wait for multiple
auto [a, b, c] = xi::await_all(f1, f2, f3);

// Check without blocking
if (future.ready()) { ... }

// Pre-wrap a function
ASYNC_WRAP(myFunction)
// Now: async_myFunction(args...) is available
```

### How it works

`xi::async` spawns a `std::async(std::launch::async, ...)` task. `Future<T>` has `operator T()` that calls `.get()` — the "implicit await" pattern. Exceptions propagate through the conversion.

### Example: parallel branches

```cpp
auto p1 = xi::async(featureA, gray);    // starts immediately
auto p2 = xi::async(featureB, gray);    // starts immediately
// both running in parallel here
Image a = p1;                            // blocks until p1 done
Image b = p2;                            // blocks until p2 done (likely already finished)
```

---

## 7. Parameters

### Header

```cpp
#include <xi/xi_param.hpp>
```

### API

```cpp
xi::Param<int>    thresh {"threshold", 128, {0, 255}};
xi::Param<double> sigma  {"sigma",     3.0, {0.1, 10.0}};
xi::Param<bool>   invert {"invert",    false};

// Read via implicit conversion
int t = thresh;          // reads atomic value
double s = sigma;

// Write (clamped to range)
thresh.set(200);

// JSON round-trip
std::string json = thresh.as_json();
thresh.set_from_json("150");
```

Parameters auto-register in `ParamRegistry` at construction. The VS Code sidebar shows them with type/value/range. `cmd: set_param` updates them without recompiling.

---

## 8. Plugin System

### Plugin folder structure

```
plugins/
  blob_analysis/
    plugin.json              manifest
    blob_analysis.dll         compiled plugin
    ui/
      index.html             web UI (optional)
  mock_camera/
    plugin.json
    mock_camera.dll
    ui/
      index.html
```

### plugin.json manifest

```json
{
  "name": "blob_analysis",
  "description": "Threshold + contour extraction",
  "dll": "blob_analysis.dll",
  "factory": "xi_plugin_create",
  "has_ui": true
}
```

### Writing a plugin (new C ABI)

```cpp
#include <xi/xi_abi.hpp>
#include <xi/xi_ops.hpp>

class MyPlugin : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        auto img = input.get_image("frame");
        auto gray = xi::ops::toGray(img);
        return xi::Record()
            .image("result", gray)
            .set("done", true);
    }

    std::string exchange(const std::string& cmd) override {
        // Handle UI commands
        return get_def();
    }

    std::string get_def() const override {
        return "{\"setting\": 42}";
    }

    bool set_def(const std::string& json) override {
        // Restore settings
        return true;
    }
};

XI_PLUGIN_IMPL(MyPlugin)
```

`XI_PLUGIN_IMPL` generates 6 `extern "C"` entry points: `xi_plugin_create`, `xi_plugin_destroy`, `xi_plugin_process`, `xi_plugin_exchange`, `xi_plugin_get_def`, `xi_plugin_set_def`.

### Using a plugin from a script (PluginHandle)

```cpp
#include <xi/xi_plugin_handle.hpp>

xi::PluginHandle detector{"det0", "blob_analysis"};

void xi_inspect_entry(int frame) {
    auto result = detector.process(xi::Record()
        .image("gray", gray_img)
        .set("threshold", 128));

    int blobs = result["blob_count"].as_int();
    auto binary = result.get_image("binary");
}
```

`PluginHandle` auto-discovers the DLL in `plugins/<name>/`, loads it via `LoadLibrary`, resolves all C ABI entry points, and marshals `Record` ↔ `xi_record` transparently.

### Plugin UI (web-based)

Plugin UIs are HTML files in `plugins/<name>/ui/index.html`. They communicate with the backend via `postMessage`:

```javascript
// Send a command to the plugin instance
vscode.postMessage({ type: 'exchange', cmd: { command: 'set_threshold', value: 128 } });

// Receive status updates
window.addEventListener('message', (e) => {
    if (e.data.type === 'status') { /* update UI */ }
    if (e.data.type === 'preview') { /* render image */ }
    if (e.data.type === 'process_result') { /* show results */ }
});
```

---

## 9. C ABI Reference

### Header

```c
#include <xi/xi_abi.h>    // C types
#include <xi/xi_abi.hpp>   // C++ wrapper
```

### Image handles

Images cross the DLL boundary as opaque `uint32_t` handles managed by the host's image pool.

```c
typedef uint32_t xi_image_handle;
#define XI_IMAGE_NULL 0
```

### Host API (provided by backend to plugins)

```c
typedef struct xi_host_api {
    xi_image_handle (*image_create)(int32_t w, int32_t h, int32_t channels);
    void            (*image_addref)(xi_image_handle h);
    void            (*image_release)(xi_image_handle h);
    uint8_t*        (*image_data)(xi_image_handle h);
    int32_t         (*image_width)(xi_image_handle h);
    int32_t         (*image_height)(xi_image_handle h);
    int32_t         (*image_channels)(xi_image_handle h);
    int32_t         (*image_stride)(xi_image_handle h);
    void            (*log)(int32_t level, const char* msg);
} xi_host_api;
```

### Record at the C boundary

```c
typedef struct {
    const char*      key;
    xi_image_handle  handle;
} xi_record_image;

typedef struct {
    const xi_record_image* images;
    int32_t                image_count;
    const char*            json;
} xi_record;
```

### Plugin exports

```c
void* xi_plugin_create(const xi_host_api* host, const char* name);
void  xi_plugin_destroy(void* inst);
void  xi_plugin_process(void* inst, const xi_record* input, xi_record_out* output);
int   xi_plugin_exchange(void* inst, const char* cmd, char* rsp, int rsplen);
int   xi_plugin_get_def(void* inst, char* buf, int buflen);
int   xi_plugin_set_def(void* inst, const char* json);
```

### ABI stability

- All types are plain C structs with fixed layout
- No C++ types (no std::string, no vtable) cross the boundary
- Safe across MSVC versions, potentially across compilers
- The C++ wrapper (`xi::Plugin`, `XI_PLUGIN_IMPL`) hides all marshaling

---

## 10. Image Pool

### Header

```cpp
#include <xi/xi_image_pool.hpp>
```

The image pool manages refcounted image buffers. All images that cross plugin boundaries go through the pool.

### Lifecycle

```
host->image_create(w, h, ch)  → handle (refcount=1)
host->image_addref(handle)     → refcount++    (cache it)
host->image_data(handle)       → pixel pointer  (read/write)
host->image_release(handle)    → refcount--     (done with it)
                                  refcount=0 → freed
```

### Thread safety

- `image_data/width/height/channels/stride`: `shared_lock` (concurrent reads OK)
- `image_create`: `unique_lock`
- `image_addref`: `shared_lock` + `atomic` increment
- `image_release` (refcount→0): `unique_lock`, erase, delete outside lock
- Memory ordering: `memory_order_acq_rel` on final release

### Within same MSVC build

If you trust the ABI (same compiler), `xi::Image` uses `shared_ptr` internally — zero-copy between plugins without the pool. The pool is for cross-compiler safety.

---

## 11. WebSocket Protocol

Connection: `ws://127.0.0.1:7823` (configurable)

### Text messages (JSON)

| Type | Direction | Purpose |
|------|-----------|---------|
| `cmd` | client→backend | Send a command |
| `rsp` | backend→client | Command response |
| `vars` | backend→client | Tracked variables after a run |
| `instances` | backend→client | Instance/param registry state |
| `log` | backend→client | Log messages |
| `event` | backend→client | Out-of-band notifications |

### Commands

| Command | Args | Description |
|---------|------|-------------|
| `ping` | — | Health check |
| `version` | — | Backend version |
| `shutdown` | — | Stop backend |
| `compile_and_load` | `{path}` | Compile .cpp → .dll, load |
| `unload_script` | — | Unload current script |
| `run` | — | Execute one inspection |
| `start` | `{fps}` | Start continuous mode |
| `stop` | — | Stop continuous mode |
| `set_param` | `{name, value}` | Update a parameter |
| `list_params` | — | Get all params |
| `list_instances` | — | Get all instances |
| `set_instance_def` | `{name, def}` | Update instance config |
| `exchange_instance` | `{name, cmd}` | Send command to instance |
| `save_instance_config` | `{name}` | Persist instance to disk |
| `preview_instance` | `{name}` | Grab JPEG frame from source |
| `process_instance` | `{name, source, params}` | Run plugin process via C ABI |
| `list_plugins` | — | Discovered plugins |
| `load_plugin` | `{name}` | Load a plugin DLL |
| `create_project` | `{folder, name}` | Create project structure |
| `open_project` | `{folder}` | Open existing project |
| `create_instance` | `{name, plugin}` | Create plugin instance |
| `get_project` | — | Get project state |
| `get_plugin_ui` | `{plugin}` | Get UI folder path |
| `save_project` | `{path}` | Save project to file |
| `load_project` | `{path}` | Load project from file |

### Binary messages (image preview)

```
[4B gid BE][4B codec BE][4B width BE][4B height BE][4B channels BE][JPEG bytes]
```

Header = 20 bytes. Codec: 0=JPEG, 1=BMP, 2=PNG.

GID ranges:
- 100-7999: inspection variable images
- 8000-8999: process_instance output images
- 9000-9999: config UI preview images

---

## 12. Project Structure

```
my_project/
  project.json                 project config
  inspection.cpp               user's inspection script
  instances/
    cam0/
      instance.json            {"plugin":"mock_camera","config":{...}}
    detector0/
      instance.json            {"plugin":"blob_analysis","config":{...}}
```

### project.json

```json
{
  "name": "my_project",
  "script": "inspection.cpp",
  "instances": [
    {"name": "cam0", "plugin": "mock_camera"},
    {"name": "detector0", "plugin": "blob_analysis"}
  ]
}
```

### Persistence flow

- `cmd: save_project` → writes project.json + each instance.json
- `cmd: save_instance_config` → writes one instance.json
- `cmd: open_project` → reads project.json, scans instances/, recreates via plugin factories, restores configs
- `cmd: load_project` → reads a saved file, restores params + instance defs via cJSON

---

## 13. VS Code Extension

### Activation

Extension activates on startup, spawns `xinsp-backend.exe`, connects via WebSocket.

### UI Components

| Component | Type | Purpose |
|-----------|------|---------|
| Activity Bar icon (beaker) | View Container | Opens the xInsp2 sidebar |
| Instances & Params | Tree View | Shows all instances and tunable params |
| Viewer | Webview | Variable table + image preview + Record tree |
| Plugin UI panels | Webview Panel | Per-instance config (mock_camera controls, blob analysis canvas) |
| CodeLens | Code Annotations | Clickable ⚙/🎚/👁 above Instance/Param/VAR declarations |

### Commands

| Command | Trigger | Action |
|---------|---------|--------|
| `xinsp2.compile` | Palette or auto-on-save | Compile active .cpp |
| `xinsp2.run` | Palette or toolbar | Run inspection once |
| `xinsp2.start` / `stop` | Palette | Continuous mode |
| `xinsp2.createProject` | Palette | Create project folder |
| `xinsp2.createInstance` | Palette | Add plugin instance |
| `xinsp2.openInstanceUI` | CodeLens click or tree | Open plugin config panel |

### Auto-compile on save

Saving any `.cpp` file triggers `compile_and_load` + `run` automatically.

### CodeLens annotations

```cpp
⚙ Configure cam0                    ← click opens config UI
xi::PluginHandle blobs{"det0", "blob_analysis"};

🎚 Tune threshold                    ← click focuses sidebar
xi::Param<int> thresh{"threshold", 128, {0, 255}};

👁 Preview gray                      ← click highlights in viewer
    VAR(gray, toGray(img));
```

---

## 14. Build System

### Backend

```bash
cd backend/build
cmake .. -G "Visual Studio 18 2026" -A x64
cmake --build . --config Release
# Outputs: xinsp-backend.exe, test_xi_core.exe, test_protocol.exe
```

CMake options:
- `-DXINSP2_HAS_IPP=ON` — Intel IPP JPEG encoder
- `-DXINSP2_HAS_OPENCV=ON` — OpenCV JPEG encoder (for AMD CPUs)

### Plugins

```bash
cd plugins/build
cmake .. -G "Visual Studio 18 2026" -A x64
cmake --build . --config Release
# Outputs: mock_camera.dll, blob_analysis.dll, data_output.dll
```

### VS Code Extension

```bash
cd vscode-extension
npm install
npm run build          # esbuild → out/extension.js
```

### Running

```bash
# Start backend
backend/build/Release/xinsp-backend.exe --port=7823

# Or launch via VS Code extension (auto-starts backend)
# F5 from vscode-extension/ project
```

### Tests

```bash
# C++ unit tests
backend/build/Release/test_xi_core.exe
backend/build/Release/test_protocol.exe

# Node integration tests (sequential)
cd vscode-extension
for f in test/ws_*.test.mjs test/protocol.test.mjs; do
    node --test --test-timeout=600000 "$f"
done

# VS Code E2E with screenshots
node test/runE2E.mjs

# Full pipeline E2E (13 screenshots)
node test/runPipeline.mjs
```

---

## 15. File Reference

### Core headers (`backend/include/xi/`)

| File | Purpose |
|------|---------|
| `xi.hpp` | Umbrella include |
| `xi_async.hpp` | `Future<T>`, `xi::async()`, `ASYNC_WRAP` |
| `xi_var.hpp` | `VAR()`, `ValueStore`, `VarTraits<T>` |
| `xi_param.hpp` | `Param<T>`, `ParamRegistry` |
| `xi_instance.hpp` | `Instance<T>`, `InstanceBase`, `InstanceRegistry` |
| `xi_image.hpp` | `Image` type (shared_ptr-backed pixel buffer) |
| `xi_record.hpp` | `Record` (cJSON-backed, named images + schemaless data) |
| `xi_ops.hpp` | Operator library (toGray, threshold, sobel, etc.) |
| `xi_source.hpp` | `ImageSource`, `TestImageSource` (camera interface) |
| `xi_plugin_handle.hpp` | `PluginHandle` (call any plugin from scripts via C ABI) |
| `xi_abi.h` | C ABI types (xi_record, xi_host_api, xi_image_handle) |
| `xi_abi.hpp` | C++ wrapper (`Plugin` base class, `XI_PLUGIN_IMPL`) |
| `xi_image_pool.hpp` | `ImagePool` (refcounted host-managed image storage) |
| `xi_plugin_manager.hpp` | Plugin discovery, loading, project/instance management |
| `xi_script.hpp` | Script ABI contract (export symbols) |
| `xi_script_support.hpp` | Auto-generated thunks for script DLLs |
| `xi_script_compiler.hpp` | MSVC compile driver |
| `xi_script_loader.hpp` | DLL loader + symbol resolution |
| `xi_protocol.hpp` | WS protocol types + JSON helpers |
| `xi_ws_server.hpp` | Header-only WebSocket server (RFC 6455) |
| `xi_jpeg.hpp` | JPEG encoder (IPP/OpenCV/stb dispatch) |
| `xi_project.hpp` | Project file read/write helpers |

### Plugins (`plugins/`)

| Plugin | Type | UI | Description |
|--------|------|-----|-------------|
| `mock_camera` | Image source | Yes | Simulated camera with frame counter, configurable FPS/resolution |
| `blob_analysis` | Processor | Yes | Threshold + flood-fill + contour/area/centroid/bbox |
| `data_output` | Data sink | Yes | Save results to CSV/JSON (stub) |

### Extension (`vscode-extension/`)

| File | Purpose |
|------|---------|
| `src/extension.ts` | Activation, commands, WS client, backend lifecycle |
| `src/wsClient.ts` | WebSocket client with auto-reconnect |
| `src/viewerProvider.ts` | Viewer webview (variable table + Record tree + image preview) |
| `src/instanceTree.ts` | Tree view data provider for instances/params |
| `src/instanceCodeLens.ts` | CodeLens for ⚙/🎚/👁 annotations in C++ files |
| `src/protocol.ts` | TypeScript protocol types + binary header codec |

### Vendored (`backend/vendor/`)

| File | License | Purpose |
|------|---------|---------|
| `cJSON.c` / `cJSON.h` | MIT | JSON parser used by Record |
| `stb_image_write.h` | Public domain | JPEG encoder fallback |

---

## Version History

- **43 commits** from initial architecture through current state
- M0–M7 milestones complete (core headers → protocol → WS server → vars → JPEG → compile → instances → VS Code extension)
- Plugin system with C ABI, image pool, project persistence
- Operator library, parallel execution, camera trigger model
- Automated E2E tests with VS Code + screenshots
