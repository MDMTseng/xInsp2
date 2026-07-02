# xInsp2 SDK & Plugin API Study — Authoring Mental Model & Reference

**Date:** 2026-06-30  
**Scope:** SDK surface (`sdk/`, `plugins/`, `examples/`), xi:: C++ API, plugin C ABI contract  
**Canonical sources:** `backend/include/xi/`, docs/guides/ (`write-a-plugin.md`, `write-a-script.md`)

---

## Executive Summary

xInsp2 operates a **plugin-based inspection pipeline** where C++ DLLs (plugins) run in-process, coordinated by a script (also a C++ DLL, hot-reloaded on save). The architecture is **schema-less JSON + image handles**: data flows as named-image bags (`xi::Record`) wrapped in a zero-copy, reference-counted image pool managed by the host. The bridge between script and plugins is pure C (`xi_abi.h`) — plugins export six stable C functions and receive a function-table (`xi_host_api`). Scripts call plugins via `xi::use("name").process(record)`. State persists via `xi::state()` (JSON) across hot-reloads. Parameters (`xi::Param<T>`) are tunable from the UI, slider-backed, no recompile.

---

## 1. The Authoring Mental Model

### 1.1 Three Layers

```
┌─────────────────────────────────────────────────────────┐
│  USER SCRIPT (inspect.cpp, C++, hot-reload on save)     │
│  • xi::Param<T> — tunable sliders                        │
│  • xi::use("name") → xi::UseProxy                        │
│  • xi::Record — schema-less data + named images          │
│  • xi::async(fn) — parallel branches                     │
│  • xi::state() — persistent JSON dict                    │
│  • xi::current_trigger() → event read (multi-camera)     │
│  • expose plugin — output surface (replaces VAR)         │
└─────────────────────────────────────────────────────────┘
                       ↓ xi::use()
┌─────────────────────────────────────────────────────────┐
│  PLUGINS (C++ DLLs, persist across reloads)              │
│  • class MyPlugin : public xi::Plugin                    │
│  • override process(const Record&) → Record              │
│  • override exchange(const std::string& cmd) → JSON      │
│  • pool_image(w,h,c) — allocate in host image pool      │
│  • xi::emit_record() — source plugins push frames        │
│  • optional: prepare()/commit() for frame-perfect swap   │
│  • XI_PLUGIN_IMPL(MyPlugin) — generates C ABI exports    │
└─────────────────────────────────────────────────────────┘
                  ↓ pure C ABI at boundary
┌─────────────────────────────────────────────────────────┐
│  HOST (backend, image pool, dispatch engine)             │
│  • xi_host_api* — function table to pool, logging, emit │
│  • Image pool — refcounted handles (uint64_t)            │
│  • Dispatch — triggers → script with multi-camera sync   │
│  • Hot-reload — unload script, re-call set_state()      │
└─────────────────────────────────────────────────────────┘
```

### 1.2 Instance, Plugin, Script — Three Things

- **Plugin** (DLL): A reusable operator, e.g. `blob_detect.dll`. Doesn't run until instantiated.
- **Instance**: A configured use of a plugin, e.g. `detector0` (params + on-disk state in `instances/<name>/`). Lives across script reloads.
- **Script** (DLL): User-authored inspection logic. Reloaded on `Ctrl+S`. Calls instances via `xi::use("instance_name")`.

### 1.3 Data Flow — Records & Handles, Not Pixels

- **Images are handles** (`xi_image_handle` = `uint64_t`), never raw buffers. Host owns the image pool; plugins get handles.
- **Record** = named-image map + JSON metadata. Zero-copy when forwarding unchanged images (addref, no memcpy).
- **One copy per genuinely-new image** on the way out of a plugin (unavoidable).
- **RGB, not BGR** — xInsp2 uses RGB pixel order (stb_image source); OpenCV defaults to BGR. Swap before `imwrite` / `imshow`.

### 1.4 Hot Reload Lifecycle

```
[user saves inspect.cpp]
    ↓
[cl.exe compiles → DLL]
    ↓
[backend: unload old script DLL]
    ↓
[xi_script_get_state()] — JSON serialization
    ↓
[load new DLL]
    ↓
[xi_script_set_state(json)] — restore state from JSON
    ↓
[xi_script_set_param() for each Param<T>] — slider values
    ↓
[ready for next cmd:run / cmd:start]
```

Instances and plugins **do not reload**; only the script reloads. Instances survive with their saved config.

### 1.5 Concurrency Model

- **Default (non-reentrant)**: Host serializes `process()` / `set_def()` / `exchange()` per instance with a mutex. Safe by default; no locks in plugin.
- **Reentrant** (`"reentrant": true` in `plugin.json`): Host does NOT serialize. Script can call the same instance from N dispatch workers at once. Plugin must be thread-safe (atomics, locks, or stateless).
- **Double-slot config swap** (`XI_PLUGIN_STAGED`): Heavy-resource plugins implement `prepare(def, folder)` (ungated, loads into staging) + `commit()` (gated, swaps to live). Avoids stalling the pipeline during model reload.

---

## 2. Primitive API Reference

### 2.1 Script Side — `xi::` Namespace

#### Records & Images

| Primitive | Signature | File:Line | Purpose |
|---|---|---|---|
| `xi::Record` | `class Record { Record& set(key, val); Record& image(key, img); Record& push(key, val); val as_int/as_bool/as_double/as_string(); Record& operator[](path); }` | `xi_record.hpp:*` | Schema-less JSON + named images. Path expressions (`"a.b[0].c"`), nested objects/arrays. |
| `xi::Image` | `class Image { width, height, channels; data(); empty(); as_cv_mat(); static create_in_pool(host, w,h,c); static adopt_pool_handle(host, h); }` | `xi_image.hpp:*` | Owning, refcounted 8-bit image. Refcount via host API. |
| `xi::HostImage` | `class HostImage { from_image(host, img); from_handle(host, h); share_handle(host, h); }` | `xi_abi.hpp:49–143` | Handle-backed Image for plugins. Refcount via `image_addref/release`. |

#### Parameters & Registry

| Primitive | Signature | File:Line | Purpose |
|---|---|---|---|
| `xi::Param<T>` | `template<T> Param { Param(name, default_val, Range{min, max}); operator T(); set_from_json(s); }` | `xi_param.hpp:*` | Tunable slider, range-checked. Persisted in `project.json`. Registered on construct, auto-restored on script reload. |
| `xi::Range` | `template<T> struct Range { T min, max; }` | `xi_param.hpp:39–44` | Slider bounds for Param<T>. |

#### Script Execution Context

| Primitive | Signature | File:Line | Purpose |
|---|---|---|---|
| `xi_inspect_entry(int frame)` | `XI_SCRIPT_EXPORT void xi_inspect_entry(int frame);` | `xi_script_support.hpp:*` | Entry point. Frame count starts at 0; increments per `cmd:run`. Required export (marked with `XI_SCRIPT_EXPORT`). |
| `xi::current_trigger()` | `Trigger current_trigger();` | `xi_use.hpp:101–200` | Read event metadata (multi-camera). Check `is_active()` in continuous mode. |
| `xi::Trigger` | `class Trigger { is_active(); id() / id_string(); timestamp_us(); dequeued_at_us(); image(name); sources(); has_source(name); meta(); }` | `xi_use.hpp:101–200` | Event view — frame, metadata doc, trigger id. |
| `xi::state()` | `Record& state();` | `xi_state.hpp:*` | Persistent JSON dict, survives hot-reload + restart. |
| `xi::now_us()` | `int64_t now_us();` | `xi_use.hpp:57` | Microseconds (system_clock) since Unix epoch. Same clock as trigger timestamps. |

#### Instance Access

| Primitive | Signature | File:Line | Purpose |
|---|---|---|---|
| `xi::use(name)` | `UseProxy& use(const char* name);` | `xi_use.hpp:*` | Proxy to backend-managed instance. Survives script reload. |
| `xi::UseProxy` | `class UseProxy { Record process(const Record& in); std::string exchange(const std::string& cmd); }` | `xi_use.hpp:*` | Routes to instance's C ABI (zero-copy via yyjson doc pointer). |

#### Async & Parallel

| Primitive | Signature | File:Line | Purpose |
|---|---|---|---|
| `xi::async(fn)` | `template<class F> Future<R> async(F fn);` | `xi_async.hpp:*` | Fire-and-forget parallel op. Returns `Future<R>`. SEH-wrapped so exceptions surface at await. |
| `xi::await_all(f1, f2, …)` | `template<class… Fs> auto await_all(Fs…);` | `xi_async.hpp:*` | Blocking wait on multiple Futures. Returns `tuple<R1, R2, …>`. |
| `xi::Future<R>` | `template<class R> class Future { R get(); }` | `xi_async.hpp:*` | Single-use future. Blocks on `get()`. Non-reusable. |

#### Output & Verdicts

| Primitive | Signature | File:Line | Purpose |
|---|---|---|---|
| `xi::use("expose").process(rec)` | `xi::use("expose").process(xi::Record().set("$channel", "name").image(...).set(...));` | N/A | Push per-run values/images to UI. `"$channel"` selects output tab. Shipped `expose` plugin. |
| `xi::result(code, msg)` | `void result(int code, const std::string& msg);` | `xi_result.hpp:*` | Emit single verdict per run. Sign = verdict (>0 ok, <0 ng), magnitude = sub-class. Last write wins. |
| `xi::ok(class, msg)` | `void ok(int class, const std::string& msg);` | `xi_result.hpp:*` | Shorthand: `result(+class, msg)`. |
| `xi::ng(class, msg)` | `void ng(int class, const std::string& msg);` | `xi_result.hpp:*` | Shorthand: `result(-class, msg)`. |

#### File I/O

| Primitive | Signature | File:Line | Purpose |
|---|---|---|---|
| `xi::imread(path)` | `Image imread(const char* path);` | `xi_cv.hpp:*` | Load PNG/JPEG/BMP/TGA/etc. via stb_image (RGB order). Returns empty Image on failure; never throws. |
| `xi::current_frame_path()` | `const char* current_frame_path();` | `xi_io.hpp:*` | Path passed via `cmd:run frame_path=…`. Empty string if none. |

#### Helpers

| Primitive | Signature | File:Line | Purpose |
|---|---|---|---|
| `VAR(name, expr)` | `VAR(name, expr)` → `auto name = expr;` | `xi_script_support.hpp:*` | **Legacy no-op.** Expands to local declaration; publishes nothing (VAR output was removed). Use `expose` plugin instead. |
| `EMIT(name)` | `EMIT(name)` → bare reference | `xi_script_support.hpp:*` | **Legacy no-op.** |

---

### 2.2 Plugin Side — `xi::Plugin` Base Class

#### Class & Lifecycle

| Signature | File:Line | Purpose |
|---|---|---|
| `class Plugin { Plugin(const xi_host_api* host, const std::string& name); virtual ~Plugin(); const xi_host_api* host() const; const std::string& name() const; }` | `xi_abi.hpp:147–281` | Base class for all plugins. Caches host API and instance name. |
| `virtual Record process(const Record& input)` | `xi_abi.hpp:194` | Hot path — called once per frame. Transform input Record to output Record. |
| `virtual std::string exchange(const std::string& cmd_json)` | `xi_abi.hpp:195` | RPC from UI buttons / `xi::use().exchange()`. Parse JSON, return JSON. Convention: return post-command `get_def()` for mutate-and-observe. |
| `virtual std::string get_def() const` | `xi_abi.hpp:231` | Persist config. Called on project save. Returns JSON (keep small, <1KB). |
| `virtual bool set_def(const std::string& json)` | `xi_abi.hpp:232` | Restore config. Called on project load / `cmd:set_instance_def`. Returns `true` on success. |
| `virtual bool prepare(const std::string& def, const std::string& folder)` | `xi_abi.hpp:245–247` | *Optional, ABI v7.* Load new config into staging slot (ungated, concurrent with process). Must touch ONLY staging. Defaults to `set_def(def)` if not overridden. |
| `virtual void commit()` | `xi_abi.hpp:248` | *Optional, ABI v7.* Atomically swap staging → live (gated). Defaults to no-op if not overridden. |

#### Output & Status

| Signature | File:Line | Purpose |
|---|---|---|
| `void emit_binary(const void* data, int len)` | `xi_abi.hpp:161–164` | *ABI v8.* Push opaque binary frame to WS clients (plugin's contract with UI). Safe from worker thread. |
| `void status(const std::string& text)` | `xi_abi.hpp:189–191` | Publish sticky status string (`"model loaded, 3 ROIs"`). Last-value semantics. |
| `std::string folder_path() const` | `xi_abi.hpp:173–184` | Get on-disk folder for this instance (`instances/<name>/`). Persist big assets here (ML weights, calibration). |

#### Image Pool

| Signature | File:Line | Purpose |
|---|---|---|
| `Image pool_image(int w, int h, int ch)` | `xi_abi.hpp:268–270` | Allocate fresh slot in host pool, return as refcounted Image. Write cv:: output directly into `as_cv_mat()`. Zero-copy when returned from `process()`. |
| _(removed v11)_ `create_image` | — | The redundant handle-based allocator was removed; `pool_image` is the sole author-facing image allocator. |

#### Logging & Helpers

| Signature | File:Line | Purpose |
|---|---|---|
| `void log_info(const std::string& msg)` | `xi_abi.hpp:272–274` | Log to backend stderr (level 1). |
| `void log_error(const std::string& msg)` | `xi_abi.hpp:275–277` | Log to backend stderr (level 3). |
| `static std::string exchange_unknown_command(const std::string& cmd_name)` | `xi_abi.hpp:206–230` | Return uniform error: `{"error":"unknown_command","command":"<name>"}`. Use in exchange() fallthrough. |

#### Macros

| Macro | File:Line | Purpose |
|---|---|---|
| `XI_PLUGIN_IMPL(ClassName)` | `xi_abi.hpp:528–600` | Generates all 6 C ABI exports from your class. Put at bottom of `.cpp`. Wraps `create()` in try/catch. |
| `XI_PLUGIN_STAGED(ClassName)` | `xi_abi.hpp:608–617` | Opt into ABI v7 exports: `xi_plugin_prepare()` + `xi_plugin_commit()`. Use ONLY if you override `prepare()`/`commit()` with real staging logic. |

---

### 2.3 C ABI Contract — What Crosses the Boundary

**File:** `backend/include/xi/xi_abi.h`

#### Plugin Exports (6 Mandatory)

```c
void*  xi_plugin_create(const xi_host_api* host, const char* name);
void   xi_plugin_destroy(void* inst);
void   xi_plugin_process(void* inst, const xi_record* input, xi_record_out* output);
int    xi_plugin_exchange(void* inst, const char* cmd, char* rsp, int rsp_buflen);
int    xi_plugin_get_def(void* inst, char* buf, int buflen);
int    xi_plugin_set_def(void* inst, const char* json);

/* ABI stamping (generated by XI_PLUGIN_IMPL) */
int    xi_plugin_abi_version(void);
uint32_t xi_yyjson_abi(void);

/* Optional ABI v7 exports (only if XI_PLUGIN_STAGED) */
int    xi_plugin_prepare(void* inst, const char* def_json, const char* folder);
void   xi_plugin_commit(void* inst);
```

#### Host API — `xi_host_api` Function Table (v9)

26 function pointers, appended-only per version (stable after v6):

```c
/* Image pool — refcounted uint64_t handles */
xi_image_handle (*image_create)(int w, int h, int channels);
void            (*image_addref)(xi_image_handle h);
void            (*image_release)(xi_image_handle h);
uint8_t*        (*image_data)(xi_image_handle h);
int             (*image_width)(xi_image_handle h);
int             (*image_height)(xi_image_handle h);
int             (*image_channels)(xi_image_handle h);
int             (*image_stride)(xi_image_handle h);  /* width*channels, no padding */

/* Logging */
void            (*log)(int level, const char* msg);  /* 0=debug … 3=error */

/* Instance folder */
int             (*instance_folder)(const char* name, char* buf, int buflen);

/* SHM (REMOVED 2026-05, slots retained for layout stability) — always NULL */
xi_image_handle (*shm_create_image)(int w, int h, int channels);
xi_image_handle (*shm_alloc_buffer)(int size_bytes);
void            (*shm_addref)(xi_image_handle h);
void            (*shm_release)(xi_image_handle h);
int             (*shm_is_shm_handle)(xi_image_handle h);

/* File I/O (stb_image: PNG/JPEG/BMP/TGA/GIF/PSD/HDR/PIC, RGB order) */
xi_image_handle (*read_image_file)(const char* path);

/* Status publish */
void            (*set_status)(const char* source, const char* text);

/* Doc allocator (v3, γ) — for yyjson_mut_doc backing */
void*           (*doc_chunk_alloc)(size_t size);
void*           (*doc_chunk_realloc)(void* ptr, size_t size);
void            (*doc_chunk_free)(void* ptr);

/* Doc refcount (v4, γ-4) */
void            (*doc_retain)(void* doc);
void            (*doc_release)(void* doc);
int             (*doc_refcount)(void* doc);  /* Returns refcount or 0 if unregistered */

/* Dispatch — emit a record (v6 onward) */
void            (*emit_record)(const char* emitter, xi_trigger_id id,
                               const xi_record* rec, int64_t ts);

/* Binary push to WS (v8) */
void            (*emit_binary)(const void* data, int len);

/* JPEG cache (v9) */
int             (*compress_image)(const void* pixels, int w, int h, int channels,
                                  int quality, void* out, int out_cap);
```

#### Record Structure

```c
typedef struct {
    const xi_record_image* images;
    int                    image_count;
    const uint8_t*         data;        /* JSON bytes — used iff doc == NULL */
    int                    len;
    const void*            doc;         /* borrowed yyjson_mut_doc* (v3+, in-process fast path) */
} xi_record;

typedef struct {
    xi_record_image* images;
    int              image_count;
    int              image_capacity;
    const uint8_t*   data;              /* JSON bytes (tls-owned or malloc'd) — used iff out_doc == NULL */
    int              len;
    void*            out_doc;           /* yyjson_mut_doc* (v3+, adopted by caller, shared refcount v4+) */
} xi_record_out;

typedef struct {
    const char*      key;
    xi_image_handle  handle;
} xi_record_image;
```

#### Lifecycle & Contract

1. `xi_plugin_create(host, name)` — once per instance. Return opaque instance pointer. Cache `host`.
2. `xi_plugin_set_def(saved_config)` — if project has persisted config.
3. **Per cycle:** `xi_plugin_process(input, output)` zero to N times per frame (depends on dispatch threads).
4. `xi_plugin_exchange(cmd, rsp, len)` — UI button clicks or `xi::use().exchange()`.
5. `xi_plugin_get_def(buf, len)` — on project save. Return bytes written or `-needed` if buffer too small.
6. `xi_plugin_destroy()` — on project close / instance remove.

**Zero-copy:** Input images are views (addref to keep). Output images hand ownership to host. Forwarding unchanged is a bare addref.

**ABI Versioning:**
- Current: v9 (`emit_binary`, `compress_image`).
- Min compat: v6 (dispatch model broke layout; v1–v5 plugins rebuild).
- Version gated at load: plugin asks for X, host provides Y, plugin refused if X > Y.

---

## 3. The Plugin ABI Contract & Reentrancy

**File:** `docs/reference/c-abi.md` (definitive), `docs/guides/write-a-plugin.md` § Concurrency

### 3.1 Reentrancy & Safety — Which Type Are You?

| Type | Profile | What You Do |
|---|---|---|
| **T0** | Reentrant + stateless (pure `gray→blur`) | Nothing. No races. Fastest. |
| **T1** | Non-reentrant + mutable config (common) | Nothing. Host serializes `set_def` vs `process` for you. |
| **T2** | Reentrant + mutable state (counters, caches) | Lock it yourself (`std::mutex`, atomics). Reentrancy = N concurrent `process()` on one instance. |
| **T3** | Any of above + heavy asset reload | Implement `prepare()`/`commit()` double-slot (`XI_PLUGIN_STAGED`). Orthogonal to T0–T2. |

Declared via `plugin.json`: `"reentrant": true` (or `"thread_safe": true`) + optional `"max_concurrency": M` per instance in `instance.json`.

### 3.2 Frame-Perfect Config Swap (Optional, ABI v7)

For heavy resources (ML models, calibration): override `prepare()` and `commit()`, then use `XI_PLUGIN_STAGED(MyClass)`.

```cpp
class HeavyPlugin : public xi::Plugin {
    std::atomic<std::shared_ptr<const Config>> live_;  // lock-free read by process()
public:
    bool prepare(const std::string& def_json, const std::string& folder) override {
        auto staging = std::make_shared<const Config>();
        staging->load(def_json, folder);  // concurrent with process() — must not touch live_
        staging_ = staging;  // store in private staging pointer
        return true;
    }
    void commit() override {
        live_ = staging_;  // atomic swap, gated by host (no race with process)
    }
    Record process(const Record& in) override {
        auto cfg = live_.load();  // lock-free read
        // … use cfg
    }
};
XI_PLUGIN_IMPL(HeavyPlugin)
XI_PLUGIN_STAGED(HeavyPlugin)  // exports prepare+commit, tells host we'll swap
```

The host:
- Calls `prepare()` **ungated** (concurrent with in-flight `process()` calls) — so prepare must touch ONLY staging.
- Calls `commit()` **gated** (serialized, after draining dispatch).
- A plugin without `XI_PLUGIN_STAGED` falls back to gated `set_def` (prepare) / no-op (commit).

### 3.3 Lifecycle × Thread × Serialization Contract

For each C-ABI export: the lifecycle state it is legal in, the thread it runs on, its
serialization guarantee, and the ambient-context rule. Source-verified against
`xi_abi.h` + `xi_cabi_adapter.hpp`; the full prose version is in
`docs/guides/write-a-plugin.md` § *Plugin lifecycle & threading contract*.

- **`CallScope` gate** (`xi_cabi_adapter.hpp:293-311`): a per-instance admission gate.
  Non-reentrant ⇒ one call at a time across `process`/`exchange`/`get_def`/`set_def`/`commit`.
  Reentrant ⇒ lifted (up to `max_concurrency`).
- **Ambient context is `thread_local`**: the dispatch worker's `current_trigger` and the
  adapter's `OwnerGuard(owner_id_)` live only on the host's call thread; a thread the
  plugin spawns inherits neither (images → `owner=0`, `current_trigger()` empty).

| Export | State | Thread | Serialization | Ambient |
|---|---|---|---|---|
| `xi_plugin_abi_version` | pre-create | control (load gate) | n/a | none |
| `xi_plugin_create` | pre-create→live | control (`create_instance`/`open_project`) | implicit (not yet dispatchable) | `ImagePoolOwnerScope` (ctor images tagged) |
| `xi_plugin_process` | live | **dispatch worker** (`service_main.cpp:159-163`) | **gated** unless `reentrant=true` (`:242-248,286`) | `OwnerGuard` (`:244`) + worker's `current_trigger`; neither crosses spawned threads |
| `xi_plugin_exchange` | live | control (UI) or inspect (script) | **gated** (`:227-234`) | `OwnerGuard` (`:229`) |
| `xi_plugin_get_def` | live | control (save) | **gated** (`:210-217`) | `OwnerGuard` (`:212`) |
| `xi_plugin_set_def` | live | control (load/`set_instance_def`) | **gated** vs `process` (`:220-224`) | `OwnerGuard` (`:222`) |
| `xi_plugin_prepare` (v7) | live (bg) | control (`prepare_instance`) | **UNGATED — concurrent with `process`** (`:250-260`; `xi_abi.h:107-119`) | `OwnerGuard` (`:258`); **staging only** |
| `xi_plugin_commit` (v7) | live | control (`commit_group`) | **gated** (`:262-270`); group drains dispatch first | `OwnerGuard` (`:268`) |
| `xi_plugin_destroy` | live→destroyed | control (`remove_instance`/close), `~CAbiInstanceAdapter` (`:177-194`) | not gated; instance pulled from dispatch first | none; dtor sweeps via `release_all_for` (`:188`) |

**Parallelism invariants (Part I §6 / Part III §20):** (1) read ambient
(`current_trigger`, owner) on the host call thread, capture by value into your own
threads; (2) a pool image created on a spawned thread is `owner=0` — thread-safe but
outside the per-owner leak sweep; (3) no C++ exception may cross a `#pragma omp`
boundary — catch inside, rethrow on the call thread; (4) `process` may run concurrent
with `prepare`, so `prepare` writes staging only / `process` reads live only;
(5) reentrancy governs `process`-vs-`process`, not config swaps (those are always
gated). See `docs/internals/core_fix_plan.md`.

---

## 4. Examples Catalog

**Base path:** `examples/` (90+ subdirectories)

### 4.1 Script Examples — Core Inspection Patterns

| Example | File | Pattern | Key Primitives |
|---|---|---|---|
| `user_script_example.cpp` | `.cpp` | Basic params, VAR (legacy) | `xi::Param`, `VAR(name, expr)` |
| `use_demo.cpp` | `.cpp` | Instance access, xi::use() + push-model frame read | `xi::use("inst")`, `xi::current_trigger().image(...)`, `xi::state()` |
| `defect_detection.cpp` | `.cpp` | Threshold + blob count verdict | `xi::ng()`, `xi::ok()` |
| `multi_file_script` | `*/inspect.cpp` + `inspect_*.hpp` | Split logic across headers | Include pattern; each header calls xi:: primitives |
| `script_external_dll` | `inspect.cpp` | Link external library | `project.json`: `include_dirs`, `link_libs` |
| `record_demo` | `examples/` | Record nesting, arrays | `Record().set("a.b", x).push("arr", val)` |

### 4.2 Plugin Examples — Common Operators

| Example | File | Pattern | Key Methods |
|---|---|---|---|
| `mock_camera/` | `plugins/mock_camera/mock_camera.cpp` | Synthetic source (worker thread, emit_record) | `std::thread`, `xi::emit_record(host, name, rec)` |
| `blob_analysis/` | `plugins/blob_analysis/blob_analysis.cpp` | Processor: threshold + contour trace | `process()`, `get_def()`, `set_def()` |
| `expose/` | `plugins/expose/` | Output sink (records to UI) | `"sink": true` in plugin.json |
| `data_output/` | `plugins/data_output/` | Comm sink (MES/PLC) | Result ordering, `"sink": true` |
| `config_swap_probe/` | `plugins/config_swap_probe/` | Frame-perfect config swap demo | `prepare()`/`commit()` + `XI_PLUGIN_STAGED` |
| `json_source/` | `plugins/json_source/` | Emit from JSON (for replay) | `xi::emit_record()` |
| `synced_stereo/` | `plugins/synced_stereo/` | Multi-camera gathering (correlated images) | One record, N named images |
| `record_save/` | `plugins/record_save/` | Persist records to disk | File I/O in plugin |
| `record_load/` | `plugins/record_load/` | Replay from disk | Source plugin, emit replay |

### 4.3 QA / Integration Examples

| Category | Subdirectories | Focus |
|---|---|---|
| **Parallelism** | `qa_reentrancy`, `qa_dispatch_groups`, `parallel_inspect_demo`, `burst_dispatch` | Concurrency modes (T0–T3), dispatch thread tuning. |
| **Config Management** | `qa_instance_def_recompile`, `config_validation_demo`, `qa_param_state_isolation` | Persist state, param isolation across hot-reloads. |
| **Dispatch & Ordering** | `qa_result_order`, `burst_pipeline`, `multi_source_surge` | Queue depth, result order (completion vs arrival), latency splits. |
| **Multi-Camera** | `stereo_sync`, `stereo_sync2`, `synced_stereo`, `circle_counting` | Gathering plugins, trigger metadata (routing). |
| **Lifecycle** | `qa_lifecycle_teardown`, `hot_reload_run`, `hot_reload_run2` | Instance teardown, script reload safety. |
| **Error Handling** | `plugin_crash_forensics`, `crash_tests`, `qa_fault` | SEH translation, minidumps, recovery. |
| **Comm & IO** | `comms_gateway`, `comms_script`, `preview_sink_demo` | Socket, file I/O, output paths. |

### 4.4 Feature-Specific Examples

| Feature | Examples | Pattern |
|---|---|---|
| **Async / Parallel** | `use_demo`, `burst_dispatch`, `parallel_inspect_demo` | `xi::async(fn)` / `xi::await_all()`, thread pool tuning. |
| **Per-Run Output** | `expose` plugin examples in `circle_counting`, `golden_defect` | `xi::use("expose").process(rec)` with `"$channel"`. |
| **Trigger Metadata** | `trigger_metadata`, `multi_source_surge` | `xi::current_trigger().meta()`, routing context. |
| **State Persistence** | `qa_param_state_isolation`, `use_demo` | `xi::state()["key"] = val`, survives reload. |
| **External DLL** | `script_external_dll`, `dll_version_clash` | `project.json` link+include; dependency versioning. |

### 4.5 Full Application Demos

| Demo | Focus | Components |
|---|---|---|
| `blob_tracker` | End-to-end example | Script (`inspect.cpp`) + detector plugin + UI |
| `circle_counting` | Operator + instance UI | Plugin with webview (`ui/index.html`) + expose |
| `golden_defect` | Defect detection pipeline | Multi-stage (threshold, labeling, filtering) |
| `hue_tune` | Interactive parameter tuning | Sliders (`xi::Param`), instant feedback |
| `r6_p2_demo` | Production-ready inspection | Full workflow (calibration, routing, comms) |

---

## 5. Concurrency & Config-Change Safety (Deep Dive)

**Source:** `docs/guides/write-a-plugin.md` § "Concurrency & config-change safety"

### 5.1 The Lock-Free Pattern (T3)

If you want full throughput without hand-rolling a mutex, use atomic `shared_ptr<const T>`:

```cpp
class MyPlugin : public xi::Plugin {
    std::atomic<std::shared_ptr<const Config>> live_;

public:
    bool prepare(const std::string& def_json, const std::string& folder) override {
        auto cfg = std::make_shared<const Config>();
        cfg->load(def_json, folder);  // no mutation of live_ ← safe, ungated
        staging_ = cfg;
        return true;
    }

    void commit() override {
        live_ = staging_;  // atomic swap, gated
    }

    Record process(const Record& in) override {
        auto cfg = live_.load();  // lock-free snapshot read
        // cfg is immutable; safe under any concurrency
        return {};
    }
};
```

### 5.2 Caveats That Bite

- **"Reentrant ⇒ must lock" is false.** Stateless reentrant (T0) needs NO lock.
- **`binarize(threshold)` is NOT stateless.** `threshold` set via `set_def()`, read in `process()` — that IS racy state. T1 (non-reentrant) protects it; T2 (reentrant) requires a guard.
- **`prepare()` runs UNGATED.** It is deliberately concurrent with `process()` — so it must ONLY touch staging, never live state.

---

## 6. File Organization & Build Paths

### 6.1 In-Project Plugin

```
<project>/
  plugins/
    my_detector/
      plugin.json
      src/
        my_detector.cpp
      ui/
        index.html
  project.json  ← declares: plugins.my_detector = { path: "my_detector", compile: true }
```

Build: Backend compiles with `cl.exe` on save (debug flags).

### 6.2 Standalone Plugin

```
<my_plugins>/
  my_detector/
    plugin.json
    src/
      my_detector.cpp
    CMakeLists.txt
    ui/
      index.html
```

Build: User runs cmake; declares in `project.json` or `xinsp2.extraPluginDirs`.

### 6.3 Project Configuration

**`project.json` keys relevant to SDK:**

```json
{
  "name": "my_project",
  "script": "inspect.cpp",
  "plugin_dirs": ["./plugins", "${XINSP2_PLUGIN_PATH}"],  /* search roots */
  "plugins": {
    "det":   { "path": "detector", "compile": true },      /* in-project source */
    "blob":  { "path": "vision/blob" }                      /* external, prebuilt */
  },
  "include_dirs": ["include"],                             /* for script */
  "link_libs":    ["deps/foo.lib"],                        /* for script */
  "openmp_max_threads": 4,                                 /* enable OpenMP, cap to N threads */
  "parallelism": {
    "dispatch_threads": 4,
    "queue_depth": 100,
    "overflow": "drop_oldest",
    "result_order": "completion"
  }
}
```

---

## 7. Hot Reload & Instance Persistence

### 7.1 What Survives a Script Reload

- **Instances** (plugins + state).
- **`xi::state()`** JSON (serialized on unload, restored on reload).
- **`xi::Param<T>` values** (replayed).

### 7.2 What Does NOT Survive

- Static/global C++ objects in the script (DLL unloaded).
- Raw pointers held across reload (dangle).
- Caches in script globals.

**Pattern:** Use `xi::state()` for anything that needs to outlive the reload. It's a JSON dict that the host persists to disk.

---

## 8. Zero-Copy Image Forwarding

**Key insight:** A plugin that passes an input image unchanged to the next plugin pays **zero memcpy**.

```cpp
Record process(const Record& in) override {
    auto img = in.get_image("frame");  // view, refcount bump
    // [do nothing to img]
    return xi::Record().image("output", img);  // addref, hand off
}
```

The host adopts with one `addref` on the way out; the local `xi::Image` releases when it goes out of scope. Net refcount: 1, owned by the next stage.

**One copy per genuinely-new image:**

```cpp
auto dst = pool_image(src.width, src.height, 1);
cv::GaussianBlur(src.as_cv_mat(), dst.as_cv_mat(), {0,0}, 2.0);
// dst writes go straight into pool memory
return xi::Record().image("blurred", dst);  // hand off with no extra copy
```

---

## 9. Key Files by Purpose

| Purpose | Files |
|---|---|
| **C++ Script API** | `backend/include/xi/xi.hpp`, `xi_param.hpp`, `xi_record.hpp`, `xi_use.hpp`, `xi_async.hpp`, `xi_result.hpp`, `xi_state.hpp` |
| **Plugin Base Class** | `backend/include/xi/xi_abi.hpp` (C++ wrapper) |
| **C ABI Contract** | `backend/include/xi/xi_abi.h` (pure C) |
| **Plugin Impl Macro** | `xi_abi.hpp:528–600` |
| **Image Pool** | `xi_image.hpp`, `xi_image_pool.hpp` |
| **Guides** | `docs/guides/write-a-plugin.md`, `docs/guides/write-a-script.md` |
| **ABI Reference** | `docs/reference/c-abi.md` |
| **Data Types** | `docs/reference/data-types.md` |
| **Reference Plugins** | `plugins/{mock_camera, blob_analysis, expose, synced_stereo, config_swap_probe}/` |

---

## 10. Common Recipes

### 10.1 Threshold + Blob Detect (Processor Plugin)

```cpp
class BlobDetect : public xi::Plugin {
    int thresh_ = 128;

public:
    Record process(const Record& in) override {
        auto gray = in.get_image("gray");
        if (gray.empty() || gray.channels != 1) return {};

        int t = in["threshold"].as_int(thresh_);
        auto bin = pool_image(gray.width, gray.height, 1);
        for (int i = 0; i < gray.width * gray.height; ++i)
            bin.data()[i] = (gray.data()[i] > t) ? 255 : 0;

        int blobs = count_blobs(bin);
        return xi::Record()
            .image("binary", bin)
            .set("blob_count", blobs);
    }

    bool set_def(const std::string& json) override {
        // Parse JSON, update thresh_
        return true;
    }
};
XI_PLUGIN_IMPL(BlobDetect)
```

### 10.2 Synthetic Camera Source (Worker Thread)

```cpp
class MockCamera : public xi::Plugin {
    std::atomic<bool> running_{false};
    std::thread thread_;

public:
    ~MockCamera() override { stop_(); }

    std::string exchange(const std::string& cmd) override {
        if (cmd.find("\"start\"") != std::string::npos) start_();
        if (cmd.find("\"stop\"")  != std::string::npos) stop_();
        return get_def();
    }

private:
    void start_() {
        if (running_) return;
        running_ = true;
        thread_ = std::thread([this]{ run_loop(); });
    }

    void run_loop() {
        int frame = 0;
        while (running_) {
            auto img = xi::Image(640, 480, 3);  // generated frame
            xi::emit_record(host_, name().c_str(), xi::Record().image("frame", img));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            frame++;
        }
    }
};
XI_PLUGIN_IMPL(MockCamera)
```

### 10.3 Script: Threshold Tuning with expose

```cpp
#include <xi/xi.hpp>
#include <xi/xi_cv.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>

xi::Param<int> thresh{"threshold", 128, {0, 255}};

XI_INSPECT_ENTRY(t, frame) {
    auto img = t.image("frame");
    if (img.empty()) { xi::result(0, "missing frame"); return; }

    cv::Mat gray;
    cv::cvtColor(xi::as_cv_mat(img), gray, cv::COLOR_RGB2GRAY);

    int t_val = (int)thresh;  // slider
    cv::Mat bin;
    cv::threshold(gray, bin, t_val, 255, cv::THRESH_BINARY);

    int blobs = cv::connectedComponents(bin);

    // Push output
    xi::Record out;
    out.set("frame_id", frame);
    out.set("blob_count", blobs);
    out.image("binary", xi::Image(bin.cols, bin.rows, 1, bin.data));
    out.set("$channel", "main");
    xi::use("expose").process(out);

    // Verdict
    if (blobs <= 5) xi::ok(1, "clean");
    else            xi::ng(1, "too many blobs");
}
```

---

## Conclusion

The xInsp2 SDK is built around **schema-less Records carrying zero-copy image handles**. The mental model: plugins are persistent operators, instances are configured uses, scripts are hot-reloaded orchestrators. All three talk via a **stable C ABI** (`xi_abi.h`) backed by reference-counted images and JSON documents. State persists through `xi::state()`, params through `xi::Param<T>`, output through the `expose` plugin. Concurrency defaults to safe-by-default (serialized per instance) but opts into lock-free patterns (T0–T3) via per-plugin flags.

**Core primitives:**
- Script: `xi::use()`, `xi::Param<T>`, `xi::Record`, `xi::async()`, `xi::state()`.
- Plugin: `xi::Plugin::process()` / `exchange()` / `prepare()` / `commit()`.
- ABI: 6 C exports per plugin, ~26 host functions, yyjson-backed Record, refcounted images.

See `docs/guides/write-a-plugin.md` and `docs/guides/write-a-script.md` for task tours; `docs/reference/c-abi.md` for the definitive contract.
