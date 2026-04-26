# xInsp2 Framework Reference

Complete technical reference for the xInsp2 machine vision inspection framework.
62 commits. Last updated 2026-04-24 — post Phase 3 (TriggerBus + multi-camera +
recording/replay + plugin SDK).

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [User Script API](#2-user-script-api)
3. [Record — The Universal Data Container](#3-record)
4. [Image Type](#4-image-type)
5. [Operator Library (xi_ops)](#5-operator-library)
6. [Parallelism (xi_async)](#6-parallelism)
7. [Parameters (xi_param)](#7-parameters)
8. [Persistent State (xi_state)](#8-persistent-state)
9. [Backend-Managed Instances (xi_use)](#9-backend-managed-instances)
10. [Plugin System](#10-plugin-system)
11. [C ABI Reference](#11-c-abi-reference)
12. [Image Pool & Handles](#12-image-pool)
13. [Crash Isolation (SEH)](#13-crash-isolation)
14. [WebSocket Protocol](#14-websocket-protocol)
15. [Project Structure](#15-project-structure)
16. [VS Code Extension](#16-vs-code-extension)
17. [Build System](#17-build-system)
18. [File Reference](#18-file-reference)
19. [TriggerBus & Multi-Camera](#19-triggerbus)
20. [Recording & Replay](#20-recording)
21. [Plugin SDK](#21-plugin-sdk)

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
│  Image Pool (sharded, refcount) │     │    └─ blob_analysis/ui/      │
│  SEH crash isolation            │     │                              │
│  WebSocket Server               │     │                              │
└─────────────────────────────────┘     └──────────────────────────────┘
```

**Key design principles:**
- Instances are managed by the backend, not owned by scripts
- Scripts access instances via `xi::use("name")` — survives hot-reload
- Cross-frame state persists via `xi::state()` — serialized before unload, restored after
- All plugin calls are SEH-guarded — script crashes don't kill the backend
- Images cross DLL boundaries as refcounted handles via a sharded pool
- Universal C ABI for plugins — stable across compiler versions

**Data flow:**
```
User .cpp → compile → .dll → inspect() → VAR() → ValueStore
    → snapshot thunks → JSON + JPEG → WebSocket → VS Code viewer
```

---

## 2. User Script API

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
#include <xi/xi_use.hpp>

using namespace xi::ops;

xi::Param<int>    thresh {"threshold", 128, {0, 255}};
xi::Param<double> sigma  {"sigma",     2.0, {0.1, 10.0}};

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    // Access backend-managed instances (survive hot-reload)
    auto& cam  = xi::use("cam0");
    auto& det  = xi::use("detector0");

    auto img = cam.grab(500);
    if (img.empty()) return;

    VAR(input, img);
    VAR(gray, toGray(img));

    // Call plugin via C ABI — zero knowledge of plugin internals
    auto result = det.process(xi::Record()
        .image("gray", gray)
        .set("threshold", (int)thresh));

    VAR(detection, result);
    VAR(pass, result["blob_count"].as_int() <= 3);

    // Persistent state — survives hot-reload
    int count = xi::state()["count"].as_int(0);
    xi::state().set("count", count + 1);
    xi::state().set("yield", result["blob_count"].as_int() <= 3 ? 1.0 : 0.0);
}
```

### Macros

| Macro | Purpose | Example |
|-------|---------|---------|
| `VAR(name, expr)` | Track for viewer | `VAR(gray, toGray(img));` |
| `VAR_RAW(name, expr)` | Track with BMP preview | `VAR_RAW(binary, thresh(gray, t));` |
| `XI_SCRIPT_EXPORT` | DLL export decorator | `XI_SCRIPT_EXPORT void xi_inspect_entry(int)` |
| `ASYNC_WRAP(fn)` | Generate `async_fn()` | `ASYNC_WRAP(myFilter)` |
| `XI_PLUGIN_IMPL(Class)` | Generate 6 C ABI exports | `XI_PLUGIN_IMPL(MyPlugin)` |

### Multi-file scripts

```json
// project.json
{
  "script": ["main.cpp", "calibration.cpp", "tracking.cpp"],
  "include_dirs": ["./include"]
}
```

CompileRequest supports `extra_sources` and `include_dirs`.

---

## 3. Record

The universal data container. Every plugin input/output and every complex `VAR()` uses Record. Backed by cJSON internally.

### Building

```cpp
xi::Record r;
r.set("count", 5);
r.set("score", 0.95);
r.set("pass", true);
r.set("label", "ok");
r.image("input", img);

// Nested
r.set("roi", xi::Record().set("x", 100).set("y", 50));

// Arrays
r.push("items", xi::Record().set("area", 247).set("pass", true));
r.push("items", xi::Record().set("area", 95).set("pass", false));
```

### Reading (three styles, all safe with defaults)

```cpp
// Style 1: chained operator[]
r["roi"]["x"].as_int(0);
r["items"][0]["score"].as_double();

// Style 2: path expression (auto-detected by . or [)
r["roi.x"].as_int(0);
r["items[0].score"].as_double();

// Style 3: explicit .at()
r.at("roi.x").as_int(0);

// All safe — missing keys return defaults, never crash
r["a"]["b"]["c"].as_int(42);  // → 42

// Check existence
r["mask"].exists();
r["items"].size();       // array length
r["items"].is_array();
```

### Value proxy methods

| Method | Returns | Default |
|--------|---------|---------|
| `.as_int(def)` | int | 0 |
| `.as_double(def)` | double | 0.0 |
| `.as_bool(def)` | bool | false |
| `.as_string(def)` | string | "" |
| `.as_record()` | Record (deep copy) | empty |
| `.size()` | array length | 0 |
| `.exists()` | bool | — |
| `.is_null/object/array/number/string/bool()` | bool | — |

---

## 4. Image Type

```cpp
#include <xi/xi_image.hpp>

xi::Image img(640, 480, 3);           // allocate
xi::Image copy = img;                  // shared_ptr copy (zero-copy)
uint8_t* p = img.data();              // pixel pointer
int w = img.width, h = img.height, c = img.channels;
```

Layout: row-major, interleaved, uint8. Channels: 1 (gray), 3 (RGB), 4 (RGBA).
Copy = shared_ptr reference (zero-copy for pixel data).

---

## 5. Operator Library

```cpp
#include <xi/xi_ops.hpp>
using namespace xi::ops;
```

| Function | Description |
|----------|-------------|
| `toGray(img)` | RGB → grayscale (BT.601) |
| `threshold(gray, t)` | Binary threshold |
| `gaussian(gray, r)` | 3-pass box blur |
| `sobel(gray)` | Edge magnitude |
| `invert(img)` | 255 - pixel |
| `erode(gray, r)` / `dilate(gray, r)` | Morphology |
| `stats(gray)` | Mean, stddev, min, max |
| `countWhiteBlobs(binary)` | Flood-fill connected components |

Each has `async_` variant: `async_sobel(gray)` returns `Future<Image>`.

---

## 6. Parallelism

```cpp
auto p1 = xi::async(featureA, gray);    // spawn
auto p2 = xi::async(featureB, gray);    // both running
Image a = p1;                            // implicit await
Image b = p2;

auto [a, b, c] = xi::await_all(f1, f2, f3);  // wait all
```

`Future<T>` has `operator T()` — implicit await. Exceptions propagate through.

---

## 7. Parameters

```cpp
xi::Param<int>    t {"threshold", 128, {0, 255}};
xi::Param<double> s {"sigma", 3.0, {0.1, 10.0}};
int val = t;           // atomic read via implicit conversion
```

Auto-registers in `ParamRegistry`. VS Code sidebar shows with type/value/range. `cmd: set_param` updates without recompile.

---

## 8. Persistent State

```cpp
#include <xi/xi_state.hpp>

void xi_inspect_entry(int frame) {
    int count = xi::state()["count"].as_int(0);
    xi::state().set("count", count + 1);
    xi::state().set("yield", 0.95);
}
```

**Survives:**
- ✅ Across frames (normal execution)
- ✅ Across hot-reloads (serialized to JSON before unload, restored after)
- ✅ Across save/load project
- ❌ Across backend restarts (unless project is saved)

**Mechanism:** Backend calls `xi_script_get_state()` → JSON string → stores in memory → loads new DLL → calls `xi_script_set_state(json)`.

---

## 9. Backend-Managed Instances

```cpp
#include <xi/xi_use.hpp>

void xi_inspect_entry(int frame) {
    auto& cam = xi::use("cam0");       // backend owns this
    auto& det = xi::use("detector0");  // backend owns this
    auto img = cam.grab(500);
    auto result = det.process(Record().image("gray", img));
}
```

**Key difference from `PluginHandle`:**
- `PluginHandle` — script loads the DLL itself, DLL reload kills the instance
- `xi::use()` — calls back to the backend's `InstanceRegistry` via thunks, instance survives script reload

**Thread safety:** The proxy cache is mutex-protected. Multiple `xi::async` threads can call `xi::use("cam0")` concurrently.

Instances are created via VS Code UI (`cmd: create_instance`) or `cmd: open_project` (auto-restores from project.json).

---

## 10. Plugin System

### Plugin folder structure

```
plugins/
  blob_analysis/
    plugin.json           manifest
    blob_analysis.dll      compiled plugin
    ui/index.html          web UI (optional)
```

### Writing a plugin (C ABI)

```cpp
#include <xi/xi_abi.hpp>

class MyPlugin : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        auto img = input.get_image("frame");
        int t = input["threshold"].as_int(128);
        auto gray = xi::ops::toGray(img);
        return xi::Record()
            .image("result", gray)
            .set("done", true);
    }

    std::string exchange(const std::string& cmd) override { return get_def(); }
    std::string get_def() const override { return "{\"setting\":42}"; }
    bool set_def(const std::string& json) override { return true; }
};

XI_PLUGIN_IMPL(MyPlugin)
```

`XI_PLUGIN_IMPL` generates: `xi_plugin_create`, `xi_plugin_destroy`, `xi_plugin_process`, `xi_plugin_exchange`, `xi_plugin_get_def`, `xi_plugin_set_def`.

### Shipped plugins

| Plugin | Type | UI | Description |
|--------|------|-----|-------------|
| `mock_camera` | Source | ✅ | Simulated camera, FPS/resolution config, frame counter, live preview |
| `blob_analysis` | Processor | ✅ | Threshold + flood-fill + contour/area/centroid/bbox, canvas overlay |
| `data_output` | Sink | ✅ | Save results to CSV/JSON (stub) |
| `json_source` | Source | ✅ | Replays images + records from a JSON manifest |
| `record_save` | Sink | ✅ | Writes Records to disk |
| `threshold_op` | Processor | ✅ | Simple threshold operator plugin |
| `synced_stereo` | Source | ✅ | Reference multi-frame trigger source (left + right under one tid) |

---

## 11. C ABI Reference

### Image handles

```c
typedef uint32_t xi_image_handle;
#define XI_IMAGE_NULL 0
```

### Host API (backend → plugin)

```c
typedef struct xi_host_api {
    xi_image_handle (*image_create)(int32_t w, int32_t h, int32_t ch);
    void            (*image_addref)(xi_image_handle h);
    void            (*image_release)(xi_image_handle h);
    uint8_t*        (*image_data)(xi_image_handle h);
    int32_t         (*image_width/height/channels/stride)(xi_image_handle h);
    void            (*log)(int32_t level, const char* msg);
} xi_host_api;
```

### Record at boundary

```c
typedef struct { const char* key; xi_image_handle handle; } xi_record_image;
typedef struct { const xi_record_image* images; int32_t image_count; const char* json; } xi_record;
typedef struct { xi_record_image* images; int32_t image_count; int32_t image_capacity; char* json; } xi_record_out;
```

### Plugin exports

```c
void* xi_plugin_create(const xi_host_api* host, const char* name);
void  xi_plugin_destroy(void* inst);
void  xi_plugin_process(void* inst, const xi_record* in, xi_record_out* out);
int   xi_plugin_exchange(void* inst, const char* cmd, char* rsp, int rsplen);
int   xi_plugin_get_def(void* inst, char* buf, int buflen);
int   xi_plugin_set_def(void* inst, const char* json);
```

### ABI stability

All types are plain C structs. No C++ types cross the boundary. Safe across MSVC versions. The C++ wrapper (`xi::Plugin` + `XI_PLUGIN_IMPL`) hides all marshaling.

---

## 12. Image Pool

**Sharded, refcounted, thread-safe.**

16 independent shards, each with its own `shared_mutex`. Handles distributed by `handle & 0xF`.

```
host->image_create(w, h, ch)  → handle (refcount=1)
host->image_addref(handle)     → refcount++
host->image_data(handle)       → pixel pointer (shared_lock, ~50ns)
host->image_release(handle)    → refcount-- (0 → freed)
```

| Operation | Lock type | Concurrent? |
|-----------|-----------|-------------|
| data/width/height | shared_lock on 1 shard | Yes |
| addref | shared_lock + atomic | Yes |
| create | unique_lock on 1 shard | 16x less contention |
| release (→0) | unique_lock on 1 shard | delete outside lock |

**ABA prevention:** Internal counter is `uint64_t` (585 years at 1M/sec to wrap).

---

## 13. Crash Isolation

Uses `_set_se_translator` to convert Windows SEH exceptions to C++ exceptions. Normal `try/catch` catches segfaults:

```cpp
try {
    script.inspect(frame);           // user code runs here
} catch (const seh_exception& e) {
    log("crashed: 0x%08X (%s)", e.code, e.what());
    // backend continues, WS stays connected
} catch (const std::exception& e) {
    log("threw: %s", e.what());
}
```

**Protected call sites:**
- `xi_inspect_entry()` — user script inspection
- `xi_script_reset()` — script reset
- `exchange_instance` — plugin UI commands (both backend and script-DLL instances)
- `process_instance` — plugin process via C ABI
- `use_process_cb` — script calling plugin via `xi::use()`

**Tested crash types:**

| Crash | Exception code | Backend survives | Normal script after |
|-------|---------------|------------------|-------------------|
| Null pointer | 0xC0000005 ACCESS_VIOLATION | ✅ | ✅ |
| Division by zero | 0xC0000094 INT_DIVIDE_BY_ZERO | ✅ | ✅ |
| Array overrun | 0xC0000005 ACCESS_VIOLATION | ✅ | ✅ |
| C++ throw | std::runtime_error | ✅ | ✅ |
| Stack overflow | 0xC00000FD | ❌ needs guard thread | — |

Requires `/EHa` compiler flag (async exception handling).

---

## 14. WebSocket Protocol

Connection: `ws://127.0.0.1:7823` (default; configurable).

### Remote mode

By default the backend binds to loopback only. To expose it on a LAN /
factory network:

```bash
xinsp-backend.exe --host=0.0.0.0 --port=7823 --auth=<shared-secret>
#  or via env:
#   XINSP2_HOST=0.0.0.0  XINSP2_AUTH=<secret>  xinsp-backend.exe
```

Clients must send `Authorization: Bearer <secret>` in the WebSocket
handshake. Mismatches (or missing header) get `HTTP/1.1 401
Unauthorized` and the socket is closed. Compare is constant-time.

Starting with `--host=0.0.0.0` **without** `--auth` prints a prominent
stderr warning — the secret is the only access gate; TLS termination is
expected to be done by a reverse proxy (nginx, caddy) if you need it.

### Commands

| Command | Args | Description |
|---------|------|-------------|
| `ping` | — | Health check |
| `version` | — | Backend version |
| `shutdown` | — | Stop backend |
| `compile_and_load` | `{path}` | Compile .cpp → .dll, load, restore state |
| `unload_script` | — | Unload current script |
| `run` | — | Execute one inspection |
| `start` | `{fps}` | Start continuous mode |
| `stop` | — | Stop continuous mode |
| `set_param` | `{name, value}` | Update parameter |
| `list_params` | — | Get all params |
| `list_instances` | — | Get all instances |
| `set_instance_def` | `{name, def}` | Update instance config |
| `exchange_instance` | `{name, cmd}` | Send command to instance |
| `save_instance_config` | `{name}` | Persist instance to disk |
| `preview_instance` | `{name}` | Grab JPEG frame from source |
| `process_instance` | `{name, source, params}` | Run plugin process |
| `list_plugins` | — | Discovered plugins |
| `load_plugin` | `{name}` | Load plugin DLL |
| `create_project` | `{folder, name}` | Create project structure |
| `open_project` | `{folder}` | Open project (auto-loads plugins + instances) |
| `create_instance` | `{name, plugin}` | Create plugin instance |
| `get_project` | — | Get project state |
| `get_plugin_ui` | `{plugin}` | Get UI folder path |
| `save_project` / `load_project` | `{path}` | Save/load project file |
| `rescan_plugins` | — | Re-discover plugin folders on disk |
| `close_project` | — | Unload current project, clear instances |
| `remove_instance` | `{name, delete_folder}` | Destroy instance + optional disk cleanup |
| `rename_instance` | `{old_name, new_name}` | Rename + move on-disk folder |
| `recertify_plugin` | `{plugin}` | Regenerate plugin cert |
| `set_trigger_policy` | `{policy, ...}` | ANY / AllRequired / LeaderFollowers |
| `recording_start` | `{folder}` | Begin observer-mode event recording |
| `recording_stop` | — | Flush manifest + stop recording |
| `recording_status` | — | Running state + event count |
| `recording_replay` | `{folder}` | Replay events through the bus |

### Binary preview frames

```
[4B gid BE][4B codec BE][4B width BE][4B height BE][4B channels BE][JPEG bytes]
```

GID ranges: 100-7999 inspection vars, 8000-8999 process_instance, 9000-9999 config preview.

---

## 15. Project Structure

```
my_project/
  project.json                 project config (name, script, instances)
  inspection.cpp               user script (or multiple .cpp)
  instances/
    cam0/
      instance.json            {"plugin":"mock_camera","config":{...}}
    detector0/
      instance.json            {"plugin":"blob_analysis","config":{...}}
```

`open_project` auto-loads required plugins and recreates instances from saved configs.
`save_instance_config` persists current instance state to `instance.json`.

---

## 16. VS Code Extension

### UI Components

| Component | Type | Purpose |
|-----------|------|---------|
| Activity Bar (beaker) | View Container | xInsp2 sidebar |
| Instances & Params | Tree View | All instances + tunable params |
| Viewer | Webview | Variable table, Record tree, image preview, PASS/FAIL badges |
| Plugin UI panels | Webview Panel | Per-instance config (camera controls, blob canvas) |
| CodeLens | Code Annotations | ⚙ Configure / 🎚 Tune / 👁 Preview on declarations AND usages |

### Key features

- **Auto-compile on save** — saving `.cpp` triggers compile + run
- **CodeLens on pipeline code** — click `⚙` on any `xi::use("cam0")` line to open config
- **Record tree viewer** — expandable JSON + image thumbnails + PASS/FAIL badges
- **Camera live preview** — JPEG streaming at configurable FPS in the config webview
- **Blob analysis canvas** — contours, bounding boxes, centroids drawn on HTML5 canvas

---

## 17. Build System

### Backend

```bash
cd backend/build
cmake .. -G "Visual Studio 18 2026" -A x64
cmake --build . --config Release
```

Options: `-DXINSP2_HAS_IPP=ON`, `-DXINSP2_HAS_OPENCV=ON`

### Plugins

```bash
cd plugins/build
cmake .. -G "Visual Studio 18 2026" -A x64
cmake --build . --config Release
```

### Extension

```bash
cd vscode-extension && npm install && npm run build
```

### Tests

```bash
# C++ unit
backend/build/Release/test_xi_core.exe
backend/build/Release/test_protocol.exe

# Node integration (run sequentially)
cd vscode-extension
for f in test/ws_*.test.mjs test/protocol.test.mjs; do
    node --test --test-timeout=600000 "$f"
done

# Crash isolation
node --test --test-timeout=600000 test/ws_crash.test.mjs

# VS Code E2E with screenshots
node test/runE2E.mjs

# Full pipeline (13 screenshots)
node test/runPipeline.mjs
```

---

## 18. File Reference

### Core headers (`backend/include/xi/`)

| File | Purpose |
|------|---------|
| `xi.hpp` | Umbrella include |
| `xi_async.hpp` | `Future<T>`, `xi::async()`, `ASYNC_WRAP` |
| `xi_var.hpp` | `VAR()`, `ValueStore`, `VarTraits<T>` |
| `xi_param.hpp` | `Param<T>`, `ParamRegistry` |
| `xi_instance.hpp` | `Instance<T>`, `InstanceBase`, `InstanceRegistry` |
| `xi_image.hpp` | `Image` (shared_ptr pixel buffer) |
| `xi_record.hpp` | `Record` (cJSON, named images, `operator[]` chaining, path expressions) |
| `xi_ops.hpp` | Operator library (toGray, threshold, sobel, etc.) |
| `xi_source.hpp` | `ImageSource`, `TestImageSource` |
| `xi_state.hpp` | `xi::state()` — persistent cross-frame/cross-reload state |
| `xi_use.hpp` | `xi::use("name")` — proxy to backend-managed instances |
| `xi_plugin_handle.hpp` | `PluginHandle` — direct DLL loading (use `xi::use()` instead for hot-reload safety) |
| `xi_abi.h` | C ABI types (stable across compilers) |
| `xi_abi.hpp` | C++ wrapper (`Plugin`, `HostImage`, `XI_PLUGIN_IMPL`) |
| `xi_image_pool.hpp` | Sharded refcounted image pool (16 shards) |
| `xi_plugin_manager.hpp` | Plugin discovery, loading, project/instance management, `CAbiInstanceAdapter` |
| `xi_script.hpp` | Script ABI contract |
| `xi_script_support.hpp` | Auto-generated thunks + state/use callbacks |
| `xi_script_compiler.hpp` | MSVC compile driver (multi-file, vendor includes, cjson linking) |
| `xi_script_loader.hpp` | DLL loader + symbol resolution |
| `xi_protocol.hpp` | WS protocol types + JSON helpers |
| `xi_ws_server.hpp` | Header-only WebSocket server (RFC 6455) |
| `xi_jpeg.hpp` | JPEG encoder (IPP/OpenCV/stb dispatch by CPU vendor) |
| `xi_project.hpp` | Project file read/write |

### Plugins

| Plugin | ABI | UI | Description |
|--------|-----|-----|-------------|
| `mock_camera` | Old (InstanceBase*) | ✅ | Simulated camera, frame counter, FPS/resolution, live preview |
| `blob_analysis` | New (XI_PLUGIN_IMPL) | ✅ | Threshold + contour + area/centroid/bbox, canvas overlay |
| `data_output` | Old (InstanceBase*) | ✅ | Result persistence (stub) |

### Vendored

| File | License | Purpose |
|------|---------|---------|
| `cJSON.c/h` | MIT | JSON parser for Record |
| `stb_image_write.h` | Public domain | JPEG encoder fallback |

---

## Production Hardening Status

| Issue | Status | Mechanism |
|-------|--------|-----------|
| Script crashes kill backend | ✅ Fixed | SEH → C++ exception translation, `try/catch` on all call sites |
| Plugin crashes kill backend | ✅ Fixed | Same SEH protection on exchange + process |
| Image handle leak on exception | ✅ Fixed | Release on both success and error paths |
| Thread race on `xi::use()` | ✅ Fixed | Mutex-protected proxy cache |
| Worker thread dangling ref | ✅ Fixed | Pointer capture, join-before-restart |
| ImagePool ABA wraparound | ✅ Fixed | 64-bit internal counter |
| HostImage double-addref | ✅ Fixed | `from_handle()` takes ownership without addref |
| Stale instances on project switch | ✅ Fixed | Clear InstanceRegistry on open_project |
| Stack overflow | ⚠ Known | Kills process — needs guard thread (future) |
| Infinite loop in script | ⚠ Known | Needs watchdog thread (future) |
| Heap corruption from script | ⚠ Inherent | Same-process limitation — needs subprocess for full isolation |

---

## 19. TriggerBus

Upgrade path from "source pushes frame → inspect fires" to "sources emit
under a 128-bit trigger id → bus correlates → dispatch one event per id."

### Why

Hardware-triggered multi-camera capture needs the inspect routine to see
**paired** frames that belong to the same pulse. The old `ImageSource::push`
path had no notion of correlation — every push started a new inspect.

### Data types (`xi_abi.h`)

```c
typedef struct { uint64_t hi; uint64_t lo; } xi_trigger_id;
#define XI_TRIGGER_NULL (xi_trigger_id{0, 0})

void (*emit_trigger)(const char* source_name,
                     xi_trigger_id tid,
                     int64_t timestamp_us,
                     const xi_record_image* images,
                     int32_t image_count);
```

Pass `XI_TRIGGER_NULL` to ask the host to allocate a fresh id. The bus
addrefs each handle internally; the caller may release immediately.

### Policies (`xi_trigger_bus.hpp`)

| Policy | Behaviour |
|--------|-----------|
| `Any` | Fire on every emit. Default; back-compat with pre-trigger plugins. |
| `AllRequired` | Wait until every source in `required_sources` has emitted for that tid. Drop incomplete tids after `window_ms`. |
| `LeaderFollowers` | Fire on leader emit; attach followers' latest frames (best-effort). |

Configured via `cmd: set_trigger_policy`. Persisted in `project.json`
under a `trigger_policy` block:

```json
{
  "trigger_policy": {
    "mode": "AllRequired",
    "required": ["cam_left", "cam_right"],
    "window_ms": 30
  }
}
```

### Script-side API (`xi_use.hpp`)

```cpp
#include <xi/xi_use.hpp>

void xi_inspect_entry(int frame) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;

    VAR(tid,  t.id_string());
    VAR(ts,   (double)t.timestamp_us);
    VAR(left,  t.image("cam_left"));
    VAR(right, t.image("cam_right"));
}
```

Multi-frame sources use `<source>/<image_name>` keys, e.g.
`t.image("synced0/left")`.

### Bridging legacy plugins

`InstanceBase`-style sources that `push()` frames are bridged into the bus
automatically by `xi_trigger_bridge.hpp` — each push becomes an
`emit_trigger(name, fresh_tid, ...)` call. No plugin changes required.

---

## 20. Recording

`xi_trigger_recorder.hpp` attaches as an **observer** on the bus (does not
block live dispatch). Every TriggerEvent is deep-copied, pixels written to
disk, manifest updated on stop.

### On-disk layout

```
<recording-dir>/
  manifest.json
  000001_<source>.raw
  000001_<source2>.raw
  000002_<source>.raw
  ...
```

Raw header (24 bytes, little-endian):

```
magic    uint32 = 0x58494D47 ('XIMG')
version  uint32 = 1
width    uint32
height   uint32
channels uint32
reserved uint32 = 0
```

Then `width * height * channels` bytes of pixel data.

### Replay

`cmd: recording_replay` reads manifest in order and calls
`host->emit_trigger` for each event so the full pipeline (sink,
correlation policy, observers) sees events exactly as a live source would.

---

## 21. Plugin SDK

`sdk/` provides an out-of-tree plugin workflow:

### Scaffold

```bash
node %XINSP2_ROOT%\sdk\scaffold.mjs C:\dev\my_plugins\foo
cmake -S C:\dev\my_plugins\foo -B C:\dev\my_plugins\foo\build -A x64
cmake --build C:\dev\my_plugins\foo\build --config Release
```

### CMake module

`sdk/cmake/xinsp2_plugin.cmake` — one include and a single
`xinsp2_add_plugin(foo SRCS ...)` call gets you a DLL + cert + install
rules against an external xInsp2 tree pointed at by `XINSP2_ROOT`.

### Loading external plugins

- VS Code setting: `xinsp2.extraPluginDirs = ["C:\\dev\\my_plugins"]`
- CLI: `xinsp-backend.exe --plugins-dir=C:\dev\my_plugins`

### Testing helpers

- `sdk/testing/helpers.cjs` — Node client helpers for WS E2E.
- `sdk/testing/run_ui_test.mjs` — harness for plugin webview UIs.
- `sdk/template/tests/` — reference test layout shipped in every scaffold.

The plugin folder is owned by the author; xInsp2 stays read-only. Upgrade
by `git pull`-ing xInsp2 and rebuilding against the new `XINSP2_ROOT`.
