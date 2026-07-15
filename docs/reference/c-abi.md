# C ABI — plugin exports + host services

The complete, stable C contract between the host and a plugin DLL. Two halves:
the **exports a plugin provides** and the **`xi_host_api` services the host hands
back**. Canonical source: `backend/include/xi/xi_abi.h` (C) + the
`XI_PLUGIN_IMPL` macro in `xi_abi.hpp` (C++ helper). Pure C, so it stays stable
across compilers and versions.

You normally write a C++ class and let `XI_PLUGIN_IMPL` generate the C exports —
see [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) for the task tour;
this page is the exact contract.

---

## 1. Plugin exports

Every plugin DLL exports five mandatory C functions, plus the `xi.pack@1`
data-plane door:

```c
void* xi_plugin_create(const xi_host_api* host, const char* name);
void  xi_plugin_destroy(void* inst);
int   xi_plugin_exchange(void* inst, const char* cmd, char* rsp_buf, int rsp_buflen);
int   xi_plugin_get_def(void* inst, char* buf, int buflen);
int   xi_plugin_set_def(void* inst, const char* json);
/* data plane (v12): resolved by the host, NOT a fixed export — see §6.1 */
const void* xi_plugin_get_interface(const char* id, uint32_t version);
```

`XI_PLUGIN_IMPL(YourClass)` generates the five mandatory exports (plus the
`xi_plugin_abi_version` stamp — see §4); `XI_PLUGIN_PACK_DOOR(YourClass)`, placed
after it, generates `xi_plugin_get_interface` and wires the `xi.pack@1` door. A
class with these members:

```cpp
class MyPlugin {
public:
    MyPlugin(const xi_host_api* host, const char* name);
    void        process(xi::PackIn& in, xi::PackOut& out);   // the xi.pack@1 data-plane door
    std::string exchange(const std::string& cmd);
    std::string get_def() const;
    bool        set_def(const std::string& json);
};
XI_PLUGIN_IMPL(MyPlugin)                            // create/destroy/exchange/get_def/set_def
XI_PLUGIN_PACK_DOOR(MyPlugin)                       // publishes the xi.pack@1 door (§6.1)
```

(THE CUT (v12) deleted the Record `process` path — `xi_plugin_process` and the
`xi_record` containers are gone; the pack door is a plugin's sole data plane.)

Image sources are ordinary plugins too — they seal a frame pack and emit it via
`xi_pack_v1::emit_pack` (SDK: `xi::Plugin::emit(std::move(pack))`) from a worker
thread (see the `mock_camera` plugin); there is no separate source interface or
pull/`grab` model.

**Optional (ABI v7) — frame-perfect config swap.** A heavy-resource plugin (one
that loads big assets — model weights, templates, calibration — on a config
change) can opt into a two-phase swap so the load never stalls the pipeline:

```c
int  xi_plugin_prepare(void* inst, const char* def_json, const char* folder);  /* 0 = ok */
void xi_plugin_commit(void* inst);
```

Override `prepare()` / `commit()` on your class and add `XI_PLUGIN_STAGED(MyPlugin)`
*after* `XI_PLUGIN_IMPL`. `prepare()` loads the new config's assets into a
**background staging slot**; the host calls it **UNGATED** (concurrent with
`process()`), so it must touch **only** the staging slot — never live state.
`commit()` then **atomically swaps** staging → live; the host calls it **gated**
(serialized vs `process()`, and under `commit_group` after draining dispatch).
Canonical pattern: hold the live config in a `std::atomic<std::shared_ptr<const
T>>` that `process()` reads lock-free, `prepare()` builds a new one, `commit()`
swaps the pointer. A plugin that omits these falls back to gated `set_def`
(prepare) / no-op (commit), so simple plugins need neither. The orchestrator
drives them via `prepare_instance` + `commit_group` → `reference/ws-protocol.md`,
`roadmap/config-bundles-and-orchestration.md`, and `plugins/config_swap_probe/`.

*(THE CUT (v12) also deleted the optional `xi_plugin_record_schema` export —
the OQ-7b static Record field contract went with the Record plane and
`xi_record_schema.hpp`. The host no longer resolves the symbol; a plugin's
declared key/type contract now lives in `contract/plugins/<name>.decl.json`
and the codegen'd key headers.)*

**Optional — the plugin capability door `xi_plugin_get_interface` (polaris2
wave-2).** The **symmetric mirror** of the host's `get_interface`: a plugin
exports this to publish its OWN capabilities to the host, resolved via
`GetProcAddress` exactly like `prepare`/`commit` (ABI-additive —
the frozen `xi_host_api` is untouched):

```c
const void* xi_plugin_get_interface(const char* id, uint32_t version);
```

The host probes `xi_plugin_get_interface("xi.pack", 1)` → `const
xi_pack_proc_v1*` to learn a plugin does **pack-in/pack-out** on the v3
keyed-buffer Pack plane; `NULL` means the capability is absent (a plugin with no
data plane exports the symbol not at all, or returns `NULL` for that id). This is the
[synthesis §3](../new_gen/polaris2/00-synthesis.md) "pure door" dry run — a
plugin capability reached ONLY through a door rather than a new fixed export.

The matching **host** side is `get_interface("xi.pack", 1)` → `const
xi_pack_v1*`: the Pack value-type ABI (build / read / retain-release / emit a
Pack, all through an OPAQUE `xi_pack_handle` + accessor functions, never raw
struct layout). A pack plugin does NOT write these by hand — it overrides
`process(xi::PackIn&, xi::PackOut&)` and publishes the door with
`XI_PLUGIN_PACK_DOOR(Class)` after `XI_PLUGIN_IMPL`. Since THE CUT (v12) deleted
the Record `process` path, this door is the plugin's sole data plane. A contract failure
(missing/wrong entry) is a normal sealed pack stamped with a `$fault` reason
code (`contract/canonical-profile-notes.md` § "Pack-shaped fail-loud"), not
`XI_PACK_NULL` (which is reserved for a hard internal failure). The exact
vtables (incl. the capability plane that rides the same door) are in **§6**
below; see also `docs/new_gen/07-uniform-keyed-buffer-plane.md`,
`docs/new_gen/08-polaris2-main-plan.md` (Wave 2), and the pilot pair
`plugins/mock_camera` (pack-mode emit) + `plugins/blob_analysis` (pack door).

### Lifecycle

```
scan plugin.json → LoadLibrary → resolve 6 exports → ABI/yyjson load gate (§4)
  → xi_plugin_create(host, name)              once per instance
  → xi_plugin_set_def(saved_config)           if persisted
  ── per cycle: process (xi.pack@1 door) / xi_plugin_exchange ──
  → xi_plugin_get_def(buf,len)                on project save
  → xi_plugin_destroy(inst)                   on remove / close / shutdown
```

The plugin **survives script reload** (only the script DLL reloads).

---

## 2. Function-by-function contract

**`xi_plugin_create(host, name)`** — once per instance. Cache `host`; `name` is
the instance name (`"cam0"`). Return your opaque instance pointer. The macro
wraps it in `try { … } catch (…) { return nullptr; }`, so a throwing constructor
fails the load cleanly (host logs "factory returned null", skips the instance,
the project still opens).

**`xi_plugin_destroy(inst)`** — mirror of create; free what create allocated.

> **Exceptions never cross the ABI boundary.** `XI_PLUGIN_IMPL` (and
> `XI_PLUGIN_STAGED`) wrap **every** per-call export body in `try { … } catch
> (…)`, so a C++ exception thrown from your `process` / `exchange` / `get_def` /
> `set_def` / `prepare` / `commit` / destructor is caught **inside the plugin's own
> runtime** and converted to a safe sentinel — the host is never unwound through.
> This matters most for a **prebuilt** plugin built against a different MSVC runtime
> than the host: unwinding a C++ exception across the DLL boundary is UB, and this
> in-plugin catch is what makes the boundary effectively `noexcept`. On a caught
> throw: `process` leaves `output` empty, `exchange`/`get_def` return an empty
> result, `set_def`/`prepare` return the "rejected" sentinel, and the throw is
> logged to stderr. It is still a **bug** to throw out of an export — this is
> defense-in-depth, not a supported error channel; return a clean error/`false`
> instead. (The host ALSO guards its own call sites, so a same-runtime throw was
> already survivable; this closes the cross-runtime UB gap.)

**The pack door — `process(PackIn&, PackOut&)`** — the hot path, once per cycle.
It is published by `XI_PLUGIN_PACK_DOOR` and invoked through
`xi_plugin_get_interface("xi.pack", 1)` → `xi_pack_proc_v1::process` (a borrowed,
sealed input pack in; a new sealed output pack out), **not** a fixed `xi_plugin_*`
export.
- **Read** entries by key through `PackIn`. `in.image("gray")` returns a
  zero-copy `xi_pack_image` view (dims + a borrowed pool-buffer pixel span, valid
  for the call); `in.i64_or` / `in.f64` / `in.str` / `in.bin` / `in.mp` read the
  typed scalars and nested trees. Getters are **fail-closed** — absent or
  wrong-tag ⇒ `nullopt`, never coerced.
- **Build** the output through `PackOut`. `out.image(...)` copies pixels into a
  fresh pool slot; `out.adopt_image(k, w, h, c, handle)` addrefs a pool handle you
  already own (zero-copy); `out.i64` / `out.str` / `out.boolean` / `out.mp` add
  typed entries. The door seals it into a **new** `xi_pack_handle` the host takes
  ownership of (and releases).
- **Don't release the input handle** (host owns it for the call) **or the output
  handle** (host owns it after return). A contract failure (missing input, wrong
  type) is a normal sealed pack carrying a `$fault` entry (`out.fault(code, key,
  detail)`); `XI_PACK_NULL` is reserved for a hard internal failure.

Zero-copy: forwarding an unchanged image (plugin A's input mask → plugin B) is
genuinely zero-copy — an input image entry is a borrowed pool view, and
`out.adopt_image(key, w, h, c, handle)` re-forwards a pool-backed handle with an
`addref` instead of a memcpy. A plugin that **produces new pixels** pays exactly
one memcpy when `out.image(...)` lands its fresh buffer in the pool on the way out
(structurally unavoidable until operators write straight into pool slots).

**`xi_plugin_exchange(inst, cmd, rsp, len)`** — generic JSON-in/JSON-out RPC, for
UI button clicks and `xi::use("name").exchange(...)`. Return `> 0` = bytes
written (excl. NUL); `< 0` = buffer too small, `|value|` = bytes needed (host
retries). Convention: return the post-command `get_def()` so a UI/script gets a
single mutate-and-observe round-trip. For the unknown-command fallthrough use
`xi::Plugin::exchange_unknown_command(name)` → `{"error":"unknown_command",
"command":"<name>"}` (a misspelled command otherwise looks like a successful
no-op).

**`xi_plugin_get_def`** — persisted config read (on save). Same `<n / -needed>`
byte-count return contract as `exchange`. **`xi_plugin_set_def`** — config write
(on load / `cmd:set_instance_def`) — returns **`0` on success, `<0` on failure**;
it is NOT a byte count (the `XI_PLUGIN_IMPL` macro maps `bool set_def` → `0`/`-1`).
Keep the JSON small (hundreds of bytes); for bigger data use
`host->instance_folder()`.

---

## 3. The host API — `xi_host_api`

The host hands every plugin a `const xi_host_api*` at construction; null-check
tail fields (an older host may leave them `nullptr`). New capabilities arrive as
carved `get_interface` interfaces, not new struct fields (§6).

| Group | Fields | Notes |
|---|---|---|
| Image pool | `image_create` / `image_addref` / `image_release` / `image_data` / `image_width`/`height`/`channels`/`stride` | Refcounted opaque `uint64` handles; see below. |
| Logging | `log(level, msg)` | 0=debug … 3=error → backend stderr. Any thread. SDK: `xi::Plugin::log_debug/log_info/log_warn/log_error(msg)` cover all four levels. |
| Status | `set_status(source, text)` | Push a status line to the FE status channel → `reference/ws-protocol.md`. |
| Instance | `instance_folder(name, buf, len)` | Per-instance scratch dir, created before `create()`; never auto-deleted. |
| Binary push (v8) | `emit_binary(data, len)` | Push an opaque binary frame straight to connected WS clients. The host is a dumb byte pipe — the **frame format is the plugin's contract with its UI** (self-describe: channel/key + payload). Intended for the `expose` plugin shipping one atomic `XEX1` frame (values + JPEG images) per channel (no base64, no poll). Thread-safe from a worker; null on a pre-v8 host. SDK: `xi::Plugin::emit_binary(...)`. See `plugins/expose`. |
| Compress cache (v9) | `compress_image(px, w, h, c, quality, out, cap)` | JPEG-encode an image **through a host-side N-rotate cache** keyed by a content hash: the same frame compressed by several plugins (or repeatedly) is encoded ONCE globally. Lets the `expose` plugin avoid linking opencv/turbojpeg and gives free global dedup. Returns bytes written / `-needed` / 0. Pair with `emit_binary` to push the result. |
| Capability door (v10) | `get_interface(id, version)` | Resolve a carved, independently-frozen interface (`xi.pack` / `xi.imaging` / `xi.imaging_rw` / `xi.emit` / `xi.log` / `xi.preview` @1, plus the `xi.pack`@4 self-describing blob supplement — §6.1b; the retired `@3` answers NULL) or the capability planes (§6). The v12 data plane + dispatch verb both ride this door, not a struct field. |

**Dispatch (the pack plane, not a struct field).** A source emits a frame with
`xi_pack_v1::emit_pack(emitter, id, pack, ts)` — the one dispatch verb, on the
`xi.pack@1` plane resolved through `get_interface("xi.pack", 1)`, not a raw
`xi_host_api` slot. It seals a frame pack and hands it over; the host dispatches
one inspection, and the script reads it via `current_trigger().pack()`. SDK
helper: `xi::Plugin::emit(std::move(pack))` (fills `name()` for you). →
`internals/dispatch.md`, §6.1.

**Image decode is no longer a host slot.** THE CUT (v12) evicted the v1
`read_image_file` field; decoding an image file is now the `xi.image.decode`
capability (provider: `plugins/imgcodec`, stb_image behind it), reached through
the `xi.cap` funnel (§6.2). The dead `shm_*` block (v6 placeholders) and the
in-process yyjson doc slots (`doc_chunk_*`, `doc_retain`/`release`/`refcount`)
were removed at v11 and v12 respectively.

### Image pool — the refcount contract

Images are **refcounted handles**, never raw `malloc`/`free`. `image_create`
returns a handle at refcount 1 whose pixels are **zero-filled and 64-byte
aligned** (the buffer comes from a size-class recycler since 2026-07 —
`pixpool` in `xi_image_pool.hpp` — but the zeroed-on-create contract is
unchanged); `image_addref` increments, `image_release`
decrements (freed at 0; invalid handles are no-ops). `image_stride(h)` is
`width*channels` (no padding — SIMD-safe). Ownership rules:
- **Input handles** belong to the host for the `process()` call. To keep one
  across calls, `image_addref` it and `image_release` it in `destroy`.
- **Output handles** transfer to the host on `process()` return — don't release.
- Cross-plugin handoff rides the pack: an image entry is a borrowed pool view in,
  and `out.adopt_image(...)` re-forwards it with an `addref` (host-owned refcounts,
  no copy). Sealed packs are immutable and refcounted like image handles
  (`xi_pack_v1::retain`/`release`).

(The full image table + wiring is generated by `make_host_api()` in
`xi_image_pool.hpp`.)

---

## 4. ABI version + load gate

`XI_ABI_VERSION` is **12** (`backend/include/xi/xi_abi.h`). The struct was
append-only through v5; v6 broke that to remove the retired dispatch fields; v7
added only OPTIONAL *plugin* exports (not `xi_host_api` fields); v8 then v9
**appended** `emit_binary` and `compress_image`; v10 appended `get_interface`
(the capability-query door). v11 took the authorized "retire the monolith"
break — it **removed** the five dead `shm_*` slots and stopped publishing the
whole-table `xi.legacy` view. v12 is **THE CUT**: the Record `process` path is
deleted (a plugin's data plane is the `xi.pack@1` door only), `read_image_file`
is evicted to the `xi.image.decode` capability, and the in-process doc slots +
the record-emit verb are removed. Each layout break raised `XI_ABI_MIN_COMPAT`
(now **12**), so **every pre-v12 plugin is refused at load** (rebuild required —
all first-party plugins are rebuildable in-tree). Within the compatible range the
gate only refuses a plugin asking for a *newer* ABI than the host. History:

| ver | added |
|---|---|
| 1 | image pool, trigger bus, SHM (slots removed entirely in v11), `read_image_file` |
| 2 | emit/fetch resource hooks + `set_safe_state` (both removed in v6) |
| 3 | in-process doc allocator `doc_chunk_*` (γ) |
| 4 | in-process doc refcount `doc_retain`/`doc_release`/`doc_refcount` (γ-4) |
| 5 | `emit_trigger_record` — trigger metadata doc (superseded by v6) |
| 6 | dispatch collapsed to a single record-emit host verb (deleted at v12 → `xi_pack_v1::emit_pack`); `emit_trigger`, `emit_resource`, `fetch_resource`, `fetch_image`, `emit_dispatch` REMOVED (no v4 compat — rebuild all plugins). Multi-cam = gathering plugin; replay = the `cache` plugin (`plugins/cache`, class `BufferReplay`; demo `examples/buffer_replay_demo`). |
| 7 | optional plugin exports `xi_plugin_prepare(inst,def,folder)` + `xi_plugin_commit(inst)` for frame-perfect config swap (opt in via `XI_PLUGIN_STAGED`; prepare ungated, commit gated). PLUGIN exports, not host-api fields — no struct shift, older plugins still load. |
| 8 | `emit_binary(data, len)` appended to `xi_host_api` — plugin→WS binary push (e.g. the `expose` plugin's live `XEX1` frames). Additive (last field); v6/v7 plugins still load on a v8 host. |
| 9 | `compress_image(px,w,h,c,q,out,cap)` appended — host-side JPEG encode through an N-rotate content-hash cache (global dedup). Additive; older plugins still load on a v9 host. |
| 10 | `get_interface(id, min)` appended (last field) — the capability-query door for carved per-capability interfaces (`xi.imaging` / `xi.doc` / `xi.emit` / `xi.log` / `xi.preview` @1). Additive; older plugins still load. |
| 11 | **Layout break** (`docs/internals/adr-001-host-api-freeze.md` Phase 4): the five dead `shm_*` slots removed and the whole-table `xi.legacy` query retired. `XI_ABI_EXPECTED_SIZE` → 176 (22 function pointers); `XI_ABI_MIN_COMPAT` raised 6 → 11, so every pre-v11 plugin is now **refused** (rebuild required). |
| 12 | **THE CUT** (authorized break): the Record `process` path is deleted — a plugin's data plane is the `xi.pack@1` door (`xi_plugin_get_interface("xi.pack",1)` → `xi_pack_proc_v1`) only; the host `process` export and the record-emit host verb are gone (sources emit a sealed pack via `xi_pack_v1::emit_pack`). `read_image_file` evicted to the `xi.image.decode` capability; the in-process doc heap/refcount (`doc_chunk_*`, `doc_retain`/`release`/`refcount`) removed with the Record dispatch path. `XI_ABI_EXPECTED_SIZE` → **112** (14 function pointers); `XI_ABI_MIN_COMPAT` raised 11 → 12, so every pre-v12 plugin is now **refused** (rebuild required). |

**The load gate** (a plugin failing it is refused at load with a clear error,
then `FreeLibrary`'d):
- **ABI version** — `xi_plugin_abi_version()` is bounded on **both** ends: a
  plugin asking for a version *newer* than the host is refused, and so is one
  *older* than `XI_ABI_MIN_COMPAT` (currently **12** — the v12 CUT, layered on
  the v11 `shm_*` removal). A pre-v12 (or pre-versioning, treated as v1) plugin
  would dereference `xi_host_api` at stale offsets — and speaks the deleted Record
  `process` path — so it is refused rather than loaded with a warning. Bump
  `XI_ABI_MIN_COMPAT` on every future breaking layout change.

(The v3–v11 **yyjson layout** gate — `xi_yyjson_abi()` — was removed at THE CUT
along with the in-process yyjson doc path: the pack plane crosses the ABI as an
opaque handle and needs no layout stamp, so there is no second gate and
`"json_fallback"` no longer applies to a data plane.)

> **Migration note (v6).** "Rebuild all plugins" also covers your **native
> tests**: the plugin certification suite (`xi/xi_baseline.hpp` + `xi/xi_cert.hpp`
> — `xi::baseline::load_symbols` / `run_all`, `xi::cert::certify`) was removed
> with the cert gate. A test that used it must resolve the plugin's C-ABI exports
> directly: `LoadLibrary` the DLL, then `GetProcAddress` for `xi_plugin_create` /
> `xi_plugin_destroy` / `xi_plugin_get_interface` (the `xi.pack@1` door — drive
> `xi_pack_proc_v1::process` for the data plane; + `get_def`/`set_def`/`exchange`
> as needed), and assert behaviour with `xi/xi_test.hpp` (which survives). See
> `sdk/examples/counter/tests/test_counter.cpp` for the pattern.

---

## 5. Pack at the boundary

THE CUT (v12) deleted the Record container (`xi_record` / `xi_record_out` and the
in-process yyjson doc). A plugin's data plane is the **Pack**: a sealed,
immutable, refcounted `xi_pack_handle` that crosses the ABI as an **opaque handle
+ accessor functions** — never a raw C struct layout (raw layout + a hot-reload
skew is silent corruption). A source emits one with `xi_pack_v1::emit_pack`; a
processor reads a borrowed input pack and returns a new sealed output pack through
the `xi_pack_proc_v1` door. The full value-type ABI — build / read /
retain-release / emit — is **§6.1** below.

---

## 6. The `get_interface` planes — pack + capabilities (polaris2)

Everything in this section rides `get_interface` in one direction or the other
— **zero new `xi_host_api` slots**, so the frozen v11 struct and the freeze
guard are untouched. Canonical source: `xi_abi.h` (the vtables) +
`xi_pack_abi.hpp` / `xi_cap_abi.hpp` (the host implementations).

### 6.1 `xi.pack@1` (host door) — the Pack value-type ABI

`host->get_interface("xi.pack", 1)` → `const xi_pack_v1*`. Resolve once (at
`create`) and cache — the vtable address is process-stable. The pack crosses the
ABI as an **opaque handle** + accessor functions (spans in / spans out), never
raw struct layout. Two handle types:

- `xi_pack_handle` — a **sealed, immutable, refcounted** pack
  (`retain`/`release`, the Pack analogue of `image_addref`/`image_release`).
  `XI_PACK_NULL` = no pack / hard failure.
- `xi_pack_builder` — a pre-seal, single-owner builder; `builder_seal`
  **consumes** it into a pack handle (refcount 1, caller owns);
  `builder_abandon` drops an unsealed one (releasing any handles it minted).

Entry type tags cross as `XI_PACK_TAG_*` and match `xi::PackTag` 1:1 — surviving
values frozen: `I64`=0, `F64`=1, `STR`=2, `BIN`=3, `MP`=5, `BOOL`=6
(**appended**, pack-plane hardening — earlier values never move), and `BLOB`=8
(**appended**, self-describing blob plane / spec 30). The retired `IMAGE`=4 and
`TENSOR`=7 are **permanent gaps** (spec 30 deleted the image/tensor special
case; every non-scalar payload is a self-describing `BLOB` now, and `xi/image` is
a convention type in its descriptor). v1 has no accessor that returns a raw
`BLOB` value; a v1-era generic walker sees the tag and skips it — the
`xi.pack@4` supplement, §6.1b, surfaces it (and the frozen v1 image slots are
adapters over it).

```c
typedef struct xi_pack_v1 {
    /* builder (produce) */
    xi_pack_builder (*builder_new)(void);
    void (*builder_add_i64)(xi_pack_builder b, const char* key, int64_t v);
    void (*builder_add_f64)(xi_pack_builder b, const char* key, double v);
    void (*builder_add_str)(xi_pack_builder b, const char* key, const char* s, int32_t len);
    void (*builder_add_bin)(xi_pack_builder b, const char* key, const void* data, int32_t len);
    void (*builder_add_image)(xi_pack_builder b, const char* key,
                              int32_t w, int32_t h, int32_t c, const void* pixels);
    void (*builder_adopt_image)(xi_pack_builder b, const char* key,
                                int32_t w, int32_t h, int32_t c, xi_image_handle handle);
    void (*builder_add_mp)(xi_pack_builder b, const char* key, const void* mp, int32_t len);
    xi_pack_handle (*builder_seal)(xi_pack_builder b);      /* consumes b */
    void (*builder_abandon)(xi_pack_builder b);

    /* accessors (consume a sealed pack) */
    int32_t     (*count)(xi_pack_handle f);
    const char* (*key_at)(xi_pack_handle f, int32_t i, int32_t* len);
    int32_t     (*tag_at)(xi_pack_handle f, int32_t i);        /* XI_PACK_TAG_*, -1 OOB */
    int32_t     (*tag_of)(xi_pack_handle f, const char* key);  /* -1 if absent */
    int32_t     (*get_i64)(xi_pack_handle f, const char* key, int64_t* out);
    int32_t     (*get_f64)(xi_pack_handle f, const char* key, double* out);
    int32_t     (*get_str)(xi_pack_handle f, const char* key, const char** ptr, int32_t* len);
    int32_t     (*get_bin)(xi_pack_handle f, const char* key, const void** ptr, int32_t* len);
    int32_t     (*get_image)(xi_pack_handle f, const char* key, xi_pack_image* out);
    int32_t     (*get_mp)(xi_pack_handle f, const char* key, const void** ptr, int32_t* len);

    /* lifetime */
    void (*retain)(xi_pack_handle f);
    void (*release)(xi_pack_handle f);

    /* emit into host dispatch */
    void (*emit_pack)(const char* emitter, xi_trigger_id id,
                      xi_pack_handle f, int64_t ts);

    /* ---- ADDITIVE TAIL (pre-cutover): the BOOL entry type ---- */
    void    (*builder_add_bool)(xi_pack_builder b, const char* key, int32_t v);
    int32_t (*get_bool)(xi_pack_handle f, const char* key, int32_t* out);
} xi_pack_v1;
```

Contract points:

- **Getters are fail-closed.** Every getter returns 1 on success and 0 when the
  key is absent **or** its stored tag differs from the requested type — no
  silent coercion (an i64 `0`/`1` entry is *not* a bool). `str`/`bin`/`mp`/
  `image` payloads are **borrowed** spans into the pack's slab / pool buffer,
  valid until the caller's last `release` of the handle. `key_at` returns a
  borrowed, **not NUL-terminated** span (keys live raw in the slab); `count` +
  `key_at` + `tag_at` are the generic-enumeration primitives a
  producer-agnostic consumer (`expose`, `record_save`) walks.
- **`builder_add_mp` trusts.** It is for canonical bytes built by
  `xi::mp::Writer` / a trusted producer. Foreign/untrusted msgpack must go
  through the host ingress canonicalizer first
  ([`../internals/pack-plane.md`](../internals/pack-plane.md) § Ingress).
- **`emit_pack`** — a source hands a sealed pack to dispatch. The host takes
  its **own** ref for the async event, so the caller may `release` immediately
  after. `id == XI_TRIGGER_NULL` mints a fresh id; `ts == 0` stamps host-now.
  No-op until a dispatch hook is installed. It stamps **nothing** — no
  `$src`/`$prov` (an emitted pack's entry set is the producer's contract; see
  §6.2 for the door-hop stamp).
- **The additive tail + growth doctrine.** `builder_add_bool`/`get_bool` are
  appended **after** every original field, so no existing offset moves — a
  consumer built against the shorter v1 sees an identical prefix. This was
  legal only because `xi.pack@1` had not shipped beyond the tree; `xi.pack@1`
  is now **frozen at that tail** — all later growth shipped as the
  `xi.pack@4` supplement (§6.1b) per the freeze doctrine (field order frozen
  forever within a published version). This is no longer a doctrine on trust:
  both `xi_pack_v1` and `xi_pack_v4` are pinned by a build-time guard — a
  `static_assert` on each vtable's size + last-field offset in `xi_abi.h`, and
  a per-field `{offset, type}` snapshot in `test_abi_freeze.cpp` (the same
  discipline that guards `xi_host_api`). A mid-struct insert that would
  silently rewire an already-compiled pack plugin's function pointers now fails
  the build instead. NULL-check the tail entries when
  consuming a foreign table. A canonical bool entry is the single msgpack byte
  `0xc2`/`0xc3` (tag `XI_PACK_TAG_BOOL`); `v` is 0/1 and `get_bool` writes 0/1.
- **Reserved `$`-keys.** `$fault`/`$fault_key`/`$fault_detail` (fail-loud
  contract failures), `$src`/`$prov` (provenance), `$seq` (ordering identity),
  `$channel` (routing) — one home in `xi_pack_contract.hpp`; semantics in
  [`../internals/pack-plane.md`](../internals/pack-plane.md).

**The plugin-published mirror** (probed by the host, §1): `xi_pack_proc_v1` —
`xi_pack_handle (*process)(void* inst, xi_pack_handle input)`. Input is
borrowed (host owns it); the return is a **new** sealed pack the host takes
ownership of. `XI_PACK_NULL` signals hard internal failure only; a contract
failure is a normal sealed `$fault` pack. Authored via
`process(PackIn&, PackOut&)` + `XI_PLUGIN_PACK_DOOR` — see
[`../guides/write-a-plugin.md`](../guides/write-a-plugin.md).

### 6.1b `xi.pack@4` (host door) — the self-describing blob supplement

`host->get_interface("xi.pack", 4)` → `const xi_pack_v4*`; NULL on a host with
no blob plane (null-check and fall back to v1-only behavior). **The retired
`xi.pack@3`** (tensors, user-typed `type_id` blobs, `adopt_bin`) and the
never-existed `@2` both answer **NULL forever** — spec 30 replaced the per-type
supplement with one uniform **self-describing blob**. **Additive supplement, not
a replacement**: resolve it *alongside* v1 — lifetime (`retain`/`release`),
scalar entries, `emit_pack`, the generic `count`/`key_at`/`tag_at` walk, and the
**image adapter slots** (`builder_add_image`/`builder_adopt_image`/`get_image`,
now backed by `xi/image` blobs) all stay v1 verbs, and both vtables operate on
the **same** builder/handle ids. SDK plumbing: `xi::Plugin::pack4_iface()`, the
`PackIn`/`PackOut` blob verbs, and script-side `ScriptPack::get_blob` /
`ScriptPackBuilder::add_blob`/`add_image_blob` null-check it for you.

A **blob** is a pool buffer whose head describes its own payload
(`'XBD1'` magic + `u32 desc_len` + a canonical-msgpack **descriptor map** + zero
pad + a 64B-aligned **payload**). The descriptor's convention key `"t"` names the
type (`"xi/image"`, `"toolbox/type"`); the host validates only the canonical head
(the `blob_head_validate` seam), never the keys — **the core owns no type space**.

```c
typedef struct xi_pack_v4 {
    /* Mint a self-describing buffer: write the head from `desc` (a canonical
       msgpack map, `desc_len` bytes) and expose the 64B-aligned payload region
       (`payload_len` bytes) via *payload_out for IN-PLACE fill (camera DMA).
       Returns the pool handle (caller owns it until adopt+seal; then release its
       own ref — adopt addref'd), XI_IMAGE_NULL on a bad descriptor/length/OOM. */
    xi_image_handle (*blob_mint)(const void* desc, int32_t desc_len,
                                 int64_t payload_len, void** payload_out);
    /* Adopt an already-minted buffer as a Blob entry (validates the head,
       addrefs — the pack co-owns). Zero-copy. */
    int32_t (*builder_adopt_blob)(xi_pack_builder b, const char* key,
                                  xi_image_handle h);
    /* Mint + copy convenience: build a blob from `desc` + `payload` and add it. */
    int32_t (*builder_add_blob)(xi_pack_builder b, const char* key,
                                const void* desc, int32_t desc_len,
                                const void* payload, int64_t payload_len);
    /* Blob read: the descriptor map bytes + the 64B-aligned payload span, both
       borrowed. Out params each optional. 0 unless the entry is a valid BLOB. */
    int32_t (*get_blob)(xi_pack_handle f, const char* key,
                        const void** desc, int32_t* desc_len,
                        const void** payload, int64_t* payload_len);
    /* Ordinal walk: the i-th directory row (borrowed non-NUL key span, tag,
       `external` storage flag), insertion order like v1 key_at. 0 if OOB. */
    int32_t (*entry_at)(xi_pack_handle f, int32_t i, xi_pack_entry* out);
} xi_pack_v4;
```

What it adds over v1 (and why v1's shape could not express it):

- **Self-describing blobs** (`XI_PACK_TAG_BLOB`, always EXTERN) — the one
  non-scalar entry kind. `blob_mint` → fill the 64B-aligned payload in place →
  `builder_adopt_blob` is the **zero-copy producer path** (camera DMA lands at
  `base+payload_off`); `builder_add_blob` is the copy convenience.
  `get_blob` returns the descriptor + payload, both zero-copy. Decode the
  descriptor with any msgpack reader — its `"t"` key names the type. A v1-era
  generic walker sees `XI_PACK_TAG_BLOB` and skips the entry.
- **`xi/image` is a convention type** carried in a blob's descriptor
  (`{"t":"xi/image","w","h","c","dt"}`). The frozen v1 `builder_add_image` /
  `builder_adopt_image` / `get_image` slots are **door adapters** over it:
  add synthesizes the descriptor + mints/copies; `get_image` parses an
  `xi/image` blob back into `xi_pack_image`. `builder_adopt_image` **copies** a
  raw pixel handle into a headed blob (a raw buffer has no self-describing head,
  so zero-copy is impossible — the adapter copies to keep the frozen slot honest
  rather than drop the entry; new code uses `blob_mint` + DMA + `adopt_blob`).
  `tag_of` an image entry now reports `XI_PACK_TAG_BLOB`, not the retired
  `IMAGE` tag.

  > **LEGACY — SOFT-RETIRED (deprecated, still working).** These four `xi/image`
  > adapter slots (`builder_add_image` / `builder_adopt_image` / `get_image`, and
  > the script-side `ScriptPackBuilder::add_image` / `ScriptPack::get_image` that
  > delegate to them) are **deprecated**. Behaviour is unchanged and they remain
  > in the frozen v1 vtable — but each emits a **warn-once** stderr diagnostic
  > per process per slot naming the replacement. New code produces with
  > `blob_mint`/`adopt_blob` (host) or `PackOut::blob`/`blob_mint` /
  > `ScriptPackBuilder::add_image_blob` (SDK) and consumes with `xi::read_image_blob`
  > / `PackIn::image_blob` / `ScriptPack::image_blob` over an `xi/image` blob
  > (docs/new_gen/30, 31). **Hard retirement is deferred to the xInsp3 cutover**;
  > until then a vendored panel or an older plugin keeps working as-is.
- **Ordinal-explicit iteration** — `entry_at(i)` (the whole directory row —
  borrowed non-NUL-terminated key span, `tag`, `external` storage flag — in
  **one** call per index; same insertion order as v1 `key_at`/`tag_at`). The
  retired `@3` `type_id` field is gone (a blob's type lives in its descriptor).

Contract points: every getter is fail-closed like v1 (1 = success, 0 = absent
key / wrong tag / dead handle / invalid blob head); every builder verb
**returns** 1 = entry added, 0 = refused (invalid descriptor, dead
builder/handle, pool exhausted) — nothing is added on 0. Borrowed spans follow
v1's rule: valid until the caller's last `release` of the pack handle. Field
order frozen from birth; a change ships as the next version.

### 6.2 The capability plane — `xi.cap@1` / `xi.cap.provider@1`

Lib plugins (capability providers with no data plane — nothing routes to them)
register **named, pack-door-shaped** capabilities; consumers call them by name
through the host forwarding funnel. Both directions ride `get_interface`:

```c
/* provider: get_interface("xi.cap.provider", 1) */
typedef struct xi_cap_provider_v1 {
    int32_t (*register_capability)(const char* name, xi_cap_handler_fn handler, void* self);
    int32_t (*unregister_capability)(const char* name, void* self);
} xi_cap_provider_v1;

/* consumer: get_interface("xi.cap", 1) */
typedef struct xi_cap_v1 {
    int32_t (*call)(const char* name, xi_pack_handle in, xi_pack_handle* out);
    int32_t (*available)(const char* name);   /* cheap 1/0 existence probe */
} xi_cap_v1;

typedef xi_pack_handle (*xi_cap_handler_fn)(void* self, xi_pack_handle input);
```

- **Name-only registry, versioning inside the pack.** Names carry no version
  (`"xi.jpeg.encode"`, not `name@version`). Semantic versioning rides the
  reserved i64 entry `"$v"` in the *request pack* — the provider dispatches on
  it internally and answers an unsupported `$v` with a sealed `$fault` pack
  naming its supported range; an absent `$v` means the documented default. A
  request whose only entry is bool `"$probe": true` is a version/feature probe
  (answer: str `"$versions"`, no work done). `get_interface`'s version
  parameter versions only the transport vtables, frozen like every other
  published interface.
- **Registration is a lifecycle act.** It is attributed to the **calling
  instance** via the thread's owner context (so identity costs no parameter)
  and is legal only from lifecycle code — `create` / `set_def` / `prepare` /
  `commit` / `exchange` / `destroy` — never from inside a data-plane door or a
  capability handler (refused `XI_CAP_REG_ECONTEXT`). Same-owner re-register
  **overwrites** (the `reinit` rebuild path); a name held by another live
  instance is refused (`XI_CAP_REG_ETAKEN` — provider identity is
  configuration, not a race to the slot). The registry is swept per owner on
  destroy/rebuild, like leaked image handles and pack refs.
- **Handlers must be thread-safe.** Funnel calls arrive concurrently from
  multiple dispatch threads with **no** host serialization (no `CallScope`,
  unlike the gated instance doors) — an ABI-contract requirement. The handler
  runs under the lib instance's owner guard, so its pool/pack allocations are
  attributed (and swept) like any other entry point. Pack ownership matches
  the door: input borrowed, output a new sealed pack; on `XI_CAP_OK` the
  **caller owns `*out`** (release via `xi_pack_v1.release`); on any error
  `*out` is `XI_PACK_NULL`.
- **`call()` result codes** (the funnel verdicts):

  | rc | name | meaning |
  |---|---|---|
  | 0 | `XI_CAP_OK` | `*out` is the provider's sealed answer (caller owns) |
  | −1 | `XI_CAP_EUNKNOWN` | no provider under this name / bad args |
  | −2 | `XI_CAP_ECRASHED` | handler faulted mid-call — charged to the **lib** instance (its `on_fault` policy has run) |
  | −3 | `XI_CAP_EQUARANTINED` | providing instance quarantined — handler not entered |
  | −4 | `XI_CAP_ESHAPE` | provider entry unusable (no adapter / null handler) |
  | −5 | `XI_CAP_EREENTRY` | refused: the target instance is already being called **on this thread** (per-thread acyclicity — both directions: a plugin calling a capability its own instance provides, and a handler calling back up its own chain). Refused before any lock or plugin code, so the CallScope deadlock dies at the door. |

  Registration codes: `0` OK, `−1` `XI_CAP_REG_EINVAL` (null/empty name or
  handler), `−2` `XI_CAP_REG_ECONTEXT`, `−3` `XI_CAP_REG_ETAKEN`.

  > **Result-code namespaces are per-vtable.** The integers above belong to
  > `xi_cap_v1.call` alone. The script use-door's pack `process()` has its own
  > namespace where **−5 means "target is an ordered sink — use `push()`"**,
  > not reentrancy (its −1…−4 also differ in wording: no instance / crashed /
  > quarantined / no pack door). Never compare raw codes across funnels; each
  > vtable documents its own.

- Cross-thread concurrency is deliberately *not* flagged by the reentrancy
  stack (providers contract to be thread-safe); only same-thread cycles are.
- Exemplar: `plugins/imgcodec` — the first lib plugin (`"lib": true`,
  `reentrant: true`, `on_fault: refuse`), registering `xi.jpeg.encode` +
  `xi.image.decode` in its constructor and unregistering in its destructor.

### 6.3 Optional script-DLL exports (polaris2)

The script DLL's optional exports grew alongside the pack plane. Like every
script thunk they are resolved by `GetProcAddress` and **optional** — an older
script lacks them and the host degrades as noted (canonical list:
`xi_script_loader.hpp`; the exporting side is generated by
`xi_script_support.hpp` when the script uses the matching header):

| Export | Purpose | Absent ⇒ |
|---|---|---|
| `xi_script_set_use_pack_callback` | Host wires the `xi::use(name).process(ScriptPack)` pack-door callback (gate P2). | `process(ScriptPack)` yields an empty pack |
| `xi_script_set_use_push_pack_callback` | Host wires the `xi::use(sink).push(pack)` staged-push thunk. | pack push not wired |
| `xi_script_set_run_ctx_callbacks` | Host wires the explicit per-run **RunContext** thunks (A4): get/set propagation for xi-spawned workers plus the `run_id` / `frame_path` read thunks behind `xi::run_id()` / `xi::current_frame_path()` (ordering — `run_id` is the value a producer stamps as `"$seq"` before seal). Replaces the retired ambient-TLS `xi_script_set_run_id` (U3) setter. | `xi::run_id()` reads 0, `current_frame_path()` reads `""` |
| `xi_script_kv_get` / `xi_script_kv_set` | The kv channel (U2) — the script's sole cross-frame state store: the store crosses as **canonical-msgpack bytes with explicit lengths** — never NUL-terminated (msgpack contains NULs). `get` uses the grow-and-retry convention (bytes written, or `-needed`); `0` = the store is empty. | the script has no kv state store |
| `xi_script_kv_schema_version` | The kv schema stamp (`XI_KV_SCHEMA(N)`). | schema treated as 0 |
| `xi_script_kv_change` | Typed migration hook across a hot-reload (old bytes + schemas in, migrated bytes out, same `-needed` convention); declining (≤ 0) drops the prior store. | prior kv store dropped on schema mismatch |

---

## 7. Plugin manifest — `plugin.json`

```json
{ "name": "my_plugin", "description": "...", "dll": "my_plugin.dll",
  "factory": "xi_plugin_create", "has_ui": false }
```

| Field | Meaning |
|---|---|
| `name` (req) | Registered name; what `xi::use("...")` resolves against. |
| `dll` (req) | DLL filename relative to the folder; `<name>.dll` by convention. |
| `description` / `factory` / `has_ui` | Tree label / create symbol (default `xi_plugin_create`) / serve `ui/index.html` as the webview. |
| `reentrant` (alias `thread_safe`) | `process`/`exchange`/`get_def`/`set_def` are safe to call concurrently on one instance. Default `false` = host serializes per instance (so a parallel dispatch pool is safe by default). Set `true` only if internally thread-safe. **Which type are you + the caveats** (T0 stateless-free, T1 host-protected, T2 lock-it-yourself, T3 double-slot): see [`guides/write-a-plugin.md`](../guides/write-a-plugin.md) → "Concurrency & config-change safety". |
| `json_fallback` | Allow load despite a yyjson-layout mismatch (runs the slow JSON path). Default `false` = refused; see §4 + `internals/data-layer.md`. |
| `sink` (alias `"role": "sink"`) | **Ordered output sink.** A script's `xi::use("<this instance>").process(rec)` is NOT run inline during the inspect — the host **stages** it and flushes it AFTER the inspect, inside the ordered-emit gate, so the side effect (comm → PLC, `expose` push) lands in **frame (arrival) order** even under parallel dispatch (`dispatch_threads > 1`), instead of completion order. Fire-and-forget: the `process()` reply is dropped. Each delivered record is host-stamped with `"$seq"` (the frame's arrival id, == the run's wire `run_id`) for correlation; a dropped frame simply never arrives (no head-of-line gap). The shipped `expose` plugin uses this; see `sdk/examples/comm` for a comm/PLC example. |
| `build` (alias `prebuilt`) | `"source"` (default) = backend compiles the plugin's `.cpp` with `cl.exe`. `"cmake"` (or `"prebuilt": true`) = the plugin owns its build (own `CMakeLists.txt`, for external libs / CUDA); the backend loads the prebuilt `<name>.dll` and `cmd:rebuild_plugins` runs its CMake. See `guides/write-a-plugin.md` (External libraries & CUDA). |
| `abi_version` | The `XI_ABI_VERSION` compiled against (written by `cmd:export_project_plugin`); gated per §4. |
| `manifest` (object) | Machine-readable tunables + IO surface (`params`/`inputs`/`outputs`/`exchange`), passed through verbatim for tooling/agents. |

> **Flags are top-level only, and re-read on every reload.** `reentrant`/
> `thread_safe`, `sink`/`role`, `json_fallback`, `factory` are parsed as **top-level
> JSON keys** — a `"reentrant":true` buried inside the nested `manifest` example or a
> description string is **not** honoured. They are re-parsed on **all** load paths
> (full open, cl.exe hot-recompile, cmake Rebuild), so toggling one + Save/Rebuild
> changes the live dispatch behaviour (e.g. `reentrant:false` immediately re-enables
> per-instance serialization). Closing/switching a project also frees **every**
> plugin it loaded — externals and manifest-renamed plugins included — so the next
> project never reuses a stale DLL handle.

**`manifest.params` validation** (on `cmd:open_project`): each instance's
`config` is walked against the declared params; mismatches emit an
`open_project_warnings` entry (`unknown_config_key` / `type_mismatch` /
`out_of_range` / `not_in_enum`). Warnings-only — the bad value still flows to
`set_def` (which defaults it); a typo never blocks project load. Plugins without
`manifest.params` skip validation.

---

## Legacy C++ ABI

A few old plugins use `xi::InstanceBase* xi_plugin_create(const char*)` (no
`xi_plugin_destroy` export). The loader detects the C ABI by the **presence** of
`xi_plugin_destroy`; absence → the legacy C++ path. New plugins must use the C
ABI / `XI_PLUGIN_IMPL`; the C++ path is back-compat only.

## See also

- [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) — the task tour.
- [`data-types.md`](./data-types.md) — Record / Image / typed I/O at the boundary.
- [`instances.md`](./instances.md) — instance load / persist / teardown.
- `backend/include/xi/xi_abi.h` + `xi_abi.hpp` — canonical headers.
