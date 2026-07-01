> **ARCHIVED 2026-07-01 — implemented plan-of-record.** This is the full Parts I–IV
> plan as authored and executed; it is preserved verbatim (all `§` anchors intact) so
> that the many `core_fix_plan.md §N` citations across the source, tests, and docs keep
> resolving here. The individual `**Status:** … not yet implemented` lines below are the
> *original* state at authoring — the plan is now **implemented** (Part I A1/A2/B1/B2/C1–C3,
> Part II Phases 0–4 = ABI v11 大破大立, Part III G1/G2/G3.1, Part IV T0.1–T0.3 + T1.1/T1.2 +
> the §28 race nets). The small still-open residue is tracked live in
> [`../internals/core_fix_plan.md`](../internals/core_fix_plan.md); decisions in
> [`../internals/OPEN-QUESTIONS.md`](../internals/OPEN-QUESTIONS.md).

# Core Fix Plan — Parallel-Region Context Safety

**Date:** 2026-07-01
**Scope:** Three latent hazards exposed by the decision to parallelize inside `inspect.cpp` with OpenMP + `xi::async` (see project direction: imperative C++ wiring, frame-internal parallelism).
**Status:** Plan / not yet implemented.

---

## 0. Background

Composition stays imperative in `inspect.cpp`; intra-frame acceleration uses **OpenMP** (pixel fork-join) and **`xi::async`** (independent-branch fan-out). The dispatch layer already provides inter-frame parallelism via per-group worker lanes (SEDA).

Three pieces of **inspect-thread ambient context do NOT cross into worker threads** the author spawns:

1. **The current trigger** (`g_current_trigger`, `thread_local`) — read by `xi::current_trigger()` / `t.image()` / `t.meta()`.
2. **The SEH translator** (`_set_se_translator`, per-thread on MSVC) — what turns a hardware fault into a catchable `xi::seh_exception`.
3. **The image-pool owner tag** (`current_owner()`, `thread_local`) — what attributes a created image to a script/instance for the per-owner leak sweep.

`xi::async` re-establishes the SEH translator (and the cancel token) on its worker, but **not** the trigger or the owner. Raw `#pragma omp parallel` re-establishes **none of the three**. The failure modes differ sharply in severity: trigger misuse fails **silently** (bad output / distant crash); an OpenMP fault **terminates the whole backend**; a worker-created image is merely **dropped from the leak-sweep safety net** (harmless on the happy path — see Problem C).

---

## 1. Problem A — `current_trigger()` on a worker thread returns empty silently

### 1.1 Mechanism

- `g_current_trigger` is `thread_local` — `backend/src/service_main.cpp:412`.
- The dispatch worker sets it for the duration of `inspect()` via `CurrentTriggerScope` — `service_main.cpp:434` (`g_current_trigger = &ev`), cleared in the dtor `:435`.
- The accessor thunks read it directly and **guard with an early return**, e.g.:
  - `trigger_info_cb` — `service_main.cpp:557` (`if (!g_current_trigger) { *out = {{0,0},0,0,0,0}; return; }`)
  - `trigger_image_cb` — `:566` (`if (!g_current_trigger || !source) return XI_IMAGE_NULL;`)
  - `trigger_sources_cb` — `:588`, leader — `:609`

### 1.2 Failure mode

`thread_local` means the value is set **only on the dispatch thread** running this inspect. An `xi::async` task or `#pragma omp parallel` body runs on a **different** thread, where `g_current_trigger == nullptr` (the default). The thunks therefore return `XI_IMAGE_NULL` / zeroed meta / 0 sources **without error**. The parallel branch then computes on a null image → empty output or a segfault far downstream, with **nothing pointing at the real cause**. Worse to diagnose than a crash.

```cpp
// ✗ async body runs on another thread → g_current_trigger == nullptr (silent)
auto f = xi::async([]{
    auto img = xi::current_trigger().image("gray");   // XI_IMAGE_NULL
    return heavy(img);                                 // garbage / distant crash
});

// ✓ read on the inspect thread, capture by value
auto gray = xi::current_trigger().image("gray");       // valid here
auto f = xi::async([gray]{ return heavy(gray); });     // pass data, don't touch trigger
```

**Root rule:** the trigger is *ambient on the inspect thread*. Read it there; parallel regions consume captured locals only.

### 1.3 Remediation (layered — pick depth)

The thunk cannot currently distinguish **"no trigger at all"** (legitimate, e.g. a plain `cmd:run`) from **"trigger exists but I'm on the wrong thread"** (the bug). Fix that distinction first; everything else builds on it.

#### A1 — Fail loud (DO NOW, ~10 LOC, no API change)

Record the owning thread id in a **non-thread-local** atomic when the scope is active:

```cpp
// CurrentTriggerScope ctor
g_current_trigger = &ev;
g_inspect_tid.store(GetCurrentThreadId(), std::memory_order_release);  // global, non-TL
// dtor
g_current_trigger = nullptr;
g_inspect_tid.store(0, std::memory_order_release);

// in each trigger thunk's null branch
if (!g_current_trigger) {
    if (g_inspect_tid.load(std::memory_order_acquire) != 0)
        XI_FAIL("current_trigger() called off the inspect thread — "
                "read it on the inspect thread and capture into the parallel body");
    return /* empty / XI_IMAGE_NULL */;   // genuinely no trigger: preserve current semantics
}
```

`XI_FAIL` = `abort()` + message in debug; log-once in release. Converts the silent bug into a named, immediate error. **Lowest cost, highest leverage — ship first.**

#### A2 — Ergonomic correct pattern (NEXT — also fixes OpenMP)

Add `xi::trigger_snapshot()`: on the inspect thread, copy the needed image handles (addref'd) + meta into a **by-value** object whose accessors do **not** touch `thread_local`. Safe to capture into async **and** OpenMP; the addref prevents dangling.

```cpp
auto snap = xi::trigger_snapshot();                     // inspect thread
xi::parallel_for(n, [snap](int i){ use(snap.image("gray")); });   // any thread, safe
```

Make this the documented pattern for parallel access.

#### A3 — Transparent propagation into async (OPTIONAL — async only)

Mirror the existing cancel-token trick (`xi_async.hpp:244,271`): capture `g_current_trigger` (+ owner id) and re-install it inside the closure's `Scope`. Then `current_trigger()` "just works" inside an async task.
- Limits: only `xi::async` (raw OpenMP has no closure to inject into); relies on the inspect joining all futures before returning (it does, via `.get()`), else the captured pointer dangles.

#### A4 — Root cure (LONG TERM — aligns with the "ambient is dangerous" direction)

Change the entry signature to pass the trigger explicitly:

```cpp
void xi_inspect_entry(const xi::Trigger& t, int frame);   // no thread_local ambient
```

Eliminates the bug class entirely. Cost: a signature migration across every script. Track as a separate ABI/SDK change.

**Recommendation:** A1 now → A2 as the blessed pattern → evaluate A4 long term. A3 only if transparent async access is wanted.

---

## 2. Problem B — an OpenMP fault terminates the whole backend

### 2.1 Mechanism

- Windows faults (access violation `0xC0000005`, integer divide-by-zero `0xC0000094`, stack overflow `0xC00000FD`, …) are **SEH**, not C++ exceptions; a plain `catch(...)` does **not** catch them.
- `xi::install_seh_translator()` registers `_set_se_translator(seh_translator)` which throws `xi::seh_exception(code)` — `backend/include/xi/xi_seh.hpp:52-58`. This is what makes faults catchable.
- **`_set_se_translator` is per-thread on MSVC** — `xi_seh.hpp:6`. A translator installed on thread A does nothing for thread B.
- Installed by: each dispatch worker at spawn (so `run_one_inspection`'s `try { … } catch (const seh_exception&)` at `service_main.cpp:482,532` works); `xi::async` at task entry — `xi_async.hpp:254`; `xi::spawn_worker` — `xi_thread.hpp` (per `xi_seh.hpp:17`).
- **Raw `#pragma omp parallel` threads are spawned by the OpenMP runtime** → no translator.

### 2.2 Failure mode

1. A fault inside an OpenMP region is **not** converted to `seh_exception`, so `run_one_inspection`'s catch (on a different thread anyway) never sees it.
2. An unhandled SEH fault on a thread with no translator → default handler → **the whole process terminates** (no `run_error`, all in-flight inspects lost; FE supervisor respawns the backend).

> Separate hard constraint: even with a translator installed, a C++ exception **must not propagate out of a `#pragma omp` region** (OpenMP requires it caught within the same structured block). Always catch **inside** the region, set a flag, rethrow on the inspect thread.

### 2.3 What already exists

`xi_crash_dump.hpp` installs **process-wide** handlers: `SetUnhandledExceptionFilter(write_minidump)` (`:438`) and `AddVectoredExceptionHandler(first=1, veh_logger)` (`:443`). So an OpenMP fault on an untranslated thread still produces a **minidump + log** (which thread, which fault). It is **diagnosed**, just not **recoverable**.

### 2.4 Remediation

#### B1 — Blessed `xi::parallel_for` wrapper (DO NOW — no such helper exists yet)

Collapse all the boilerplate into one call so the correct form is the easy form:

```cpp
template <class F>
void parallel_for(int n, F&& body) {
    std::atomic<unsigned> fault{0};
    #pragma omp parallel
    {
        xi::install_seh_translator();                 // per-OMP-thread
        #pragma omp for
        for (int i = 0; i < n; ++i) {
            if (xi::cancellation_requested()) continue;   // watchdog-cooperative
            try { body(i); }
            catch (const xi::seh_exception& e) { fault.store(e.code); }  // catch INSIDE region
        }
    }
    if (fault.load()) throw xi::seh_exception(fault.load());   // surface on inspect thread
}
```

One call → SEH safety + cooperative cancellation + correct cross-region semantics. Document: "parallel pixel loops use `xi::parallel_for`; do not hand-write `#pragma omp`."

> Note: cancellation here only *propagates* if the token is visible on the OMP thread. The token is `thread_local` and set by `xi::async`/dispatch, not by OpenMP. For `parallel_for` invoked from the inspect thread, poll-then-skip still drains the loop quickly; for full cooperative cancel inside OMP, capture the parent token and re-install it at region entry (same pattern as A2/A3). Keep this in the helper.

#### B2 — Warm up the OpenMP pool at load (DO NOW, ~5 LOC) — raises the floor for raw pragmas

MSVC's `vcomp` uses a **persistent** thread pool. After `omp_set_num_threads` at script DLL load (`xi_script_support.hpp:35`), force the team into existence and install the translator on each worker once:

```cpp
#ifdef _OPENMP
    #pragma omp parallel
    { xi::install_seh_translator(); }   // persistent pool → installed for later regions too
#endif
```

Subsequent **raw** `#pragma omp parallel` regions reuse those threads → faults become catchable in the common case.
⚠️ **Not airtight:** nested parallelism, `omp_set_dynamic`, or growing `num_threads` later spawns fresh untranslated threads → those fall back to B3.

#### B3 — VEH backstop (ALREADY PRESENT — nothing to build)

`xi_crash_dump.hpp:438,443` guarantees any escaping fault yields a minidump + log instead of a silent exit. Treat as the diagnostic safety net for whatever B1/B2 miss.

**Recommendation:** ship B1 as the blessed path + B2 to raise the floor; rely on B3 for the residue.

---

## 3. Problem C — a worker-created pool image is dropped from the leak sweep

### 3.1 Mechanism

- An image's owner is stamped at creation: `entry->owner = current_owner()` — `backend/include/xi/xi_image_pool.hpp:137`.
- `current_owner()` reads a **`thread_local`** slot — `xi_image_pool.hpp:226` (`static thread_local ImagePoolOwnerId v = 0`).
- It is set by an `OwnerGuard` at the **boundary, on the calling thread**:
  - script inspect: `OwnerGuard sg(s.owner_id)` — `service_main.cpp:1248`.
  - every plugin `process()`: `OwnerGuard og(owner_id_)` wrapped by the adapter — `cabi_adapter.hpp:244`.
- Both the script-side `xi::Image::create_in_pool` (`xi_image.hpp:85`, via `host->image_create`) and the plugin-side host_api `image_create` funnel through the same pool `create()` → both read this thread_local.

A worker thread spawned beneath the boundary (an `xi::async` task, a `#pragma omp parallel` body, or a thread a plugin spawns inside its own `process()`) has its **own** thread_local owner = the default **0**. Images it creates are tagged `owner=0` ("anonymous / framework").

### 3.2 Failure mode — mild (unlike A and B)

This is the **least severe** of the three, and was overstated in earlier notes:

- The pool is **thread-safe**: `create`/`acquire_slot_`/`release` are mutex-guarded over the slot vector (core study §2.2). Concurrent creation from worker threads is **not a race** and does **not** corrupt pixels or refcounts.
- On the **happy path** (every create balanced by a release, or handed off and released downstream) an `owner=0` image is freed normally when its refcount hits 0 — **no leak**.
- The only real cost is the **leak-recovery safety net**: the per-owner sweep `release_all_for(owner_id)` (instance dtor `cabi_adapter.hpp:188`; script unload) reclaims images still held by a *dying owner*. It cannot attribute `owner=0` images, so a genuinely leaked one (forgotten release, or a fault before release) **survives to pool teardown** (process exit) instead of being reclaimed at unload.
- Secondary cost: `image_pool_stats` builds an `owner_id → label` map (`service_main.cpp:3270-3282`); `owner=0` images show as **anonymous**, so leak diagnosis can't point at the creating code.

So: safe in normal operation; the downside is purely **degraded leak recovery + diagnosis** on the failure path. (Note: this is distinct from instance *call* safety — calling the same instance concurrently is already protected by the per-instance `CallScope` lock unless the plugin opts into `reentrant=true`; see `cabi_adapter.hpp:100-106,244`.)

### 3.3 What blocks the fix today

`xi::async` already re-establishes the cancel token + inspect ticket on its worker (`xi_async.hpp:244,271`); owner could ride the exact same mechanism — **except there is no way for the SDK to read or set the owner across the ABI seam**:

- `xi_host_api` has **no** owner get/set entry (confirmed — not in the function-pointer table).
- `xi_script_support.hpp` injects no owner thunk (it injects `g_use_host_api_`, `g_trigger_*_fn_`, … but nothing for owner).

The owner thread_local lives **inside the backend's `ImagePool`**; script/plugin code only touches it implicitly via `image_create`. With no handle to grab, async literally cannot carry it. **Exposing owner get/set is the prerequisite for any propagation fix.**

### 3.4 Remediation (layered)

#### C1 — Expose owner get/set across the seam (prerequisite)

Two delivery channels, pick by reach:

| Channel | Covers | ABI cost |
|---------|--------|----------|
| **`g_owner_get_fn_` / `g_owner_set_fn_` script-support thunks** (same injection as `g_use_host_api_`, `xi_script_support.hpp:220`) | `inspect.cpp` `xi::async` / OpenMP — the 90% case | **none** (script-side thunk, not an ABI field) |
| **`image_owner_get` / `image_owner_set` host_api entries** | + a plugin that parallelizes *inside* `process()` and creates owned images | ABI v10 — or slot into a future CLAP-style `xi.imaging` capability so it is additive-local, not a monolithic bump |

#### C2 — Auto-propagate through `xi::async` (~4 LOC, symmetric with the cancel-token trick)

Capture the parent owner at spawn (next to `parent_ticket = current_inspect_ticket_ref()`, `xi_async.hpp:244`) and re-install it in the closure's RAII `Scope` (next to `:271`):

```cpp
uint64_t parent_owner = owner_get();          // capture at spawn
// inside the closure, alongside the existing cancel/ticket Scope:
struct OwnerScope { uint64_t prev;
    OwnerScope(uint64_t o) : prev(owner_get()) { owner_set(o); }
    ~OwnerScope() { owner_set(prev); }
} os(parent_owner);
```

Captures whatever owner is active at spawn (script during inspect, instance if spawned inside `process()`), so async-created images are attributed correctly. Fully automatic — no author action.

#### C3 — Fold owner capture into `xi::parallel_for` (Problem B, B1)

OpenMP owns its thread pool (no per-region thread-entry hook), so this can't be transparent for hand-written pragmas — but the blessed `xi::parallel_for` can read the parent owner once before the region and install an `OwnerScope` per worker inside, exactly as it already installs the SEH translator (B1). One more reason to route parallel pixel loops through the helper rather than raw `#pragma omp`.

**Recommendation:** C1-thunk + C2 first (**zero ABI change**, fixes `inspect.cpp` async — the common path). Add the host_api/`xi.imaging` entry only when a plugin genuinely needs owned images from threads it spawns itself. This is also a concrete motivator for the capability-segregation direction: owner get/set is a tiny imaging primitive that today would force a whole-ABI event but under `xi.imaging@N` would be purely additive.

---

## 4. Combined plan

| Area | DO NOW | NEXT | ALREADY THERE |
|------|--------|------|---------------|
| **trigger** | A1 — owning-tid assert → fail loud | A2 — `trigger_snapshot()`; (A4 parameterize, long term) | thunk null-guards (no crash) |
| **OpenMP/SEH** | B1 — `xi::parallel_for`; B2 — load-time warmup | (as needed) nested/dynamic hardening | B3 — crash_dump VEH → minidump |
| **image owner** | C1-thunk + C2 — async owner propagation | C3 — owner capture in `parallel_for`; host_api/`xi.imaging` entry when a plugin self-parallelizes | pool is thread-safe; `owner=0` safe on the happy path |

Best-value bundle: **`xi::parallel_for` + load-time warmup + trigger owning-tid assert + async owner propagation** (~120–230 LOC total). Moves all three hazards from "author must remember the discipline" to "the SDK handles it." None break existing APIs — old raw-OpenMP scripts keep running (just without the helper's guarantees until migrated); the owner thunks are additive (zero ABI change).

## 5. Touch list

- `backend/src/service_main.cpp` — A1: `g_inspect_tid` set/clear in `CurrentTriggerScope` (`:434-435`); `XI_FAIL` branch in trigger thunks (`:557,566,588,609`).
- `backend/include/xi/xi_use.hpp` (or SDK trigger header) — A2: `xi::trigger_snapshot()` + by-value snapshot type.
- `backend/include/xi/xi_async.hpp` — A3 (optional): capture/re-install `g_current_trigger` in the closure `Scope` (alongside `:244,271`).
- `backend/include/xi/xi_async.hpp` or a new `xi_parallel.hpp` — B1: `xi::parallel_for` (also hosts C3's per-worker `OwnerScope`).
- `backend/include/xi/xi_script_support.hpp` — B2: warmup region after `omp_set_num_threads` (`:35`), guarded by `_OPENMP`.
- `backend/include/xi/xi_script_support.hpp` — C1: inject `g_owner_get_fn_` / `g_owner_set_fn_` thunks alongside `g_use_host_api_` (`:220`); backend wraps `ImagePool::current_owner()` / `OwnerGuard`.
- `backend/include/xi/xi_async.hpp` — C2: capture parent owner at spawn + re-install via `OwnerScope` in the closure `Scope` (alongside `:244,271`).
- `backend/include/xi/xi_abi.h` — C1 (optional, later): `image_owner_get` / `image_owner_set` host_api entries (ABI v10 / `xi.imaging`) for plugin self-parallelization.
- Docs: `docs/guides/write-a-script.md` — add a "Parallelism" section pointing at `parallel_for` / `trigger_snapshot` and the inspect-thread rule.

## 6. Invariants (true regardless of which fixes land)

1. Read the trigger on the inspect thread; parallel regions consume captured locals.
2. A C++ exception must not cross a `#pragma omp` boundary — catch inside, flag, rethrow on the inspect thread.
3. Pool images created on worker threads are tagged `owner=0` (anonymous) — thread-safe and harmless with correct refcount discipline, but outside the per-owner leak sweep. To attribute correctly: set an `OwnerGuard` / `ImagePoolOwnerScope` in the worker, or allocate on the boundary thread, or rely on the C1/C2 owner-propagation fix once landed. (See Problem C; `xi_image_pool.hpp:137,226,551-575`.)

---
---

# Part II — Core Minimization & Host-API Evolution

**Date:** 2026-07-01
**Scope:** Direction-setting for "smallest possible core that lets plugins compose, with `inspect.cpp` doing the wiring." Records (a) corrections to `STUDY-core.md`, (b) what may leave core and what must not, (c) the ambient-vs-composable principle, (d) the plan to evolve `xi_host_api` from a monolith to segregated, frozen capability interfaces.
**Status:** Analysis + plan / not yet implemented.

## 7. Corrections to `STUDY-core.md` (source-verified 2026-07-01)

`STUDY-core.md` is an accurate subsystem-level tour of the **compute hot path**, but it is stale/incomplete in three ways. Verified against source:

| `STUDY-core.md` claim | Reality | Evidence |
|---|---|---|
| Image pool 65,536 slots / 40-bit gen / 1 GiB cap | ✅ correct | `xi_image_pool.hpp:80,83,120` |
| DocRegistry 16 shards by doc-ptr hash | ✅ correct | `xi_doc_registry.hpp:89` `kShards=16` |
| WS server single-client, localhost, RFC 6455 | ✅ correct | `xi_ws_server.hpp:3,8` + `single-client-busy` reject |
| No trigger correlation (gathering plugin replaces it) | ✅ correct | `xi_trigger_bus.hpp:7` |
| "ABI-v6, 6 C exports" | ❌ stale — **`XI_ABI_VERSION 9`** (min-compat 6); **8 exports** | `xi_abi.h:124,127`; export set below |
| (omits prepare/commit, emit_binary, compress_image) | ❌ missing v7/v8/v9 host services | `xi_abi.h:107-119,329,341`; `xi_cabi_adapter.hpp:169,328` |
| Command surface ≈ a handful | ❌ **~52 WS commands**; whole subsystems undocumented | `service_main.cpp` dispatch |

**Actual plugin C exports (8):** `xi_plugin_abi_version`, `create`, `destroy`, `process`, `exchange`, `get_def`, `set_def`, plus ABI-v7 `prepare`/`commit` (async heavy-asset load, optional — `xi_cabi_adapter.hpp:257-270,328-329`).

**Command areas `STUDY-core.md` omits entirely** (≈¾ of the surface):
- Working-copy / transactional edit: `commit_group`, `commit_working_copy`, `discard_working_copy` (`xi_working_copy.hpp`).
- Instance lifecycle: `create_instance`, `remove_instance`, `rename_instance`, `exchange_instance`, `prepare_instance`, `save_instance_config`.
- Plugin lifecycle: `rebuild_plugins`, `rescan_plugins`, `recompile_project_plugin`, `export_project_plugin`, `load_plugin`.
- Observability: `dispatch_stats`, `image_pool_stats`, `graph_capture`/`graph_snapshot` (`xi_graph_capture.hpp`), `crash_reports`/`clear_crash_reports`/`recent_errors` (`xi_crash_dump.hpp`), `watchdog_status`, `toolchain_health`.
- Runtime control / UI feed: `set_watchdog_ms`, `set_process_priority`, `set_timer_fps`, `set_toolchain_override`, `unload_script`, `shutdown`, `version`, `get_plugin_ui`, `get_dashboard`, `get_project`, `create_project`.

**Action:** add a "Full command surface" section + correct the ABI version to `STUDY-core.md` (or supersede it with this Part). Until then, treat `STUDY-core.md` §2 as hot-path-only.

## 8. The minimal-core thesis — two meanings of "core"

There is already a seam in the codebase: the four sink headers state their purpose as *"indirection so `xi_core` does not depend on the backend"* (`xi_binary_sink.hpp:3`, `xi_status_sink.hpp:3`, `xi_log_sink.hpp:3`, `xi_compress_sink.hpp:3`). So "core" means two different things:

- **`xi_core`** = the headers a plugin/script compiles against (`xi.hpp` umbrella → `xi_image`, `xi_record`, `xi_use`, `xi_param`, `xi_abi`, …). **This is already the minimal compose framework.** It is thin and already free of the heavy implementations (e.g. `xi.hpp` does **not** include `xi_jpeg`; the JPEG codec lives only in `service_main.cpp` + `xi_script_compiler.hpp`).
- **`xinsp-backend.exe`** = the host process that bolts WS server, compiler/toolchain, project/working-copy management, and UI-asset serving onto the compute kernel. **This is the fat part; this is what to slim.**

**Irreducible kernel (do not move):** image pool, record/doc-registry/doc-pool, plugin manager + instance registry, `xi::use()` routing, script loader/hot-reload, TriggerBus→dispatch, SEH translation. These are the physical substrate for "plugins interoperate zero-copy by handle, `inspect.cpp` wires them."

## 9. The ambient-vs-composable principle (key correction)

An earlier pass mislabeled the operator-I/O sinks (compress/log/status/binary) as "VAR-class, move to plugins." **That was wrong.** The correct test for what may leave the kernel:

> **Pervasive-internal** — needed inside most plugins regardless of pipeline shape → **ambient `host_api` capability. Keep.**
> **Boundary-wired-once** — a concern the script wires at the edges → **plugin. Move.**

- **VAR was boundary** (a result-output surface wired once) → correctly became the `expose` plugin.
- **compress / log / set_status / emit_binary are pervasive operator/UI I/O** → keep ambient. Forcing them through `xi::use()` is boilerplate hell (every plugin previews to the webUI); opening plugin→plugin to avoid that boilerplate is dependency hell (see §10).

**Why plugin→plugin does not exist (verified):** `xi_host_api` exposes **no** `use`/`process`/`call_instance` entry — only `image_*`, `doc_*`, `emit_record`, `emit_binary`, `set_status`, `log`, `instance_folder`, `read_image_file`, `compress_image` (+ dead `shm_*` stubs). `xi::use()` relies on script-only globals (`g_use_process_fn_` et al. in `xi_script_support.hpp`), invisible to plugin DLLs. Composition is **deliberately** the script's job, one level deep (script→plugin, never plugin→plugin) — that flatness is what prevents a transitive plugin-dependency DAG. The `compress_image` comment (`xi_abi.h:346`, *"a plugin that needs JPEG can use this instead of linking its own codec"*) confirms it was made a host service **precisely because** plugins cannot `xi::use("jpeg")`.

**Consequence for compress:** keep `compress_image` ambient. The minimal-core win for it is *already banked* by the sink seam — the codec (opencv/turbojpeg/stb) is not in `xi_core`; the backend installs `encode_jpeg` at startup behind `xi_compress_sink` (`service_main.cpp:4106`), swappable. Pluginizing it would only drop the encoder from the backend link (marginal) while costing the zero-wiring convenience. **Do not pluginize compress/log/status.** (If ever extracted: would need handle-keyed dedup — key on `pool_handle()` (slot+gen), O(1) vs the current O(pixels) FNV-1a content hash at `service_main.cpp:4112` — valid only under an "images are immutable after emit" contract, which holds de facto: `Image::data()` is `const`, the pool has no in-place rewrite API; the one escape hatch is `as_cv_mat()`'s `const_cast` at `xi_image.hpp:147`.)

**Real slim targets** (not pervasive primitives → genuinely host/editor services): compiler/toolchain (`xi_script_compiler`, `xi_cmake_build`, toolchain commands), project/working-copy management (`xi_working_copy`, project CRUD), webUI asset serving (`get_dashboard`, `get_plugin_ui`). These can be externalized/pluginized without boilerplate or dependency hell, because no plugin depends on them internally. Control-plane stays on the existing WS+yyjson protocol — do not migrate it into the ABI.

## 10. The host-API evolution problem

`xi_host_api` is **one** struct of function pointers passed to every plugin. Any change is a global event. Three coupling sources:

1. **Monolith** — all capabilities share **one** `XI_ABI_VERSION`. Adding one (emit_binary v8, compress v9) bumps the whole world.
2. **Layout** — it is a struct; offsets matter. The guard `static_assert(offsetof(xi_host_api, compress_image) == XI_ABI_EXPECTED_SIZE - sizeof(void*), …)` (`xi_abi.h:376`) is the symptom: any non-append change reshuffles offsets → every plugin recompiles. (This is why the 5 dead `shm_*` stubs can't simply be deleted.)
3. **Monotonic gate** — `XI_ABI_MIN_COMPAT` is a single global threshold; capabilities cannot freeze at independent versions.

Today's scheme (append-only + min-compat) keeps **old plugins running** but freezes at **whole-API granularity** — too coarse: you can only append, never change/remove, and every append is global.

## 11. Target pattern — capability segregation + per-interface freeze

The goal is **version freeze at the granularity of one capability**, not the whole API. Mature precedents (audio-plugin world is the closest analog — in-proc, real-time, C ABI, third-party, decades of ABI-evolution pain):

| Pattern | Exemplar | Mechanism |
|---|---|---|
| Named/versioned **extension query** | **CLAP** `host->get_extension(id)` | string-keyed, each extension independently versioned/frozen — *closest match to the `xi_host_query` design* |
| **QueryInterface** | **VST3** (migrated from VST2's fat `AEffect` struct) | `queryInterface(IID)`; new version = new IID — *exact "your-pain → cure" case history* |
| **Feature handshake** | **LV2** `LV2_Feature[]` | host hands capability array; plugin declares required/optional; missing required → clean refuse |
| Per-symbol freeze | **glibc** symbol versioning | same symbol, multiple ABI versions coexist; old binaries bind old forever |
| enum-keyed single entry | **libcurl** `setopt(OPTION,…)` | one entry keyed by an only-grows enum — no struct, no layout |
| Thin-C-waist, append-only | **SQLite**, Hourglass (Du Toit, CppCon'14) | disciplined never-remove C ABI |

Principle refs: Interface Segregation (SOLID); Hyrum's Law (narrow explicit contracts); Greg KH "stable API nonsense" (stable *outward*, churn *inward*).

**Chosen blend for xInsp2:** CLAP-style `get_interface(id, version)` skeleton + LV2-style required/optional load handshake + curl-style enum entry for scalar one-offs + keep the thin-C-waist discipline. Hot-path capabilities → frozen extensions; control-plane → existing WS+yyjson. Two evolution tracks, kept separate.

## 12. Migration plan — monolith → segregated, with no breaking step

**Phase 0 — freeze discipline + safety nets (cheap, do first; no ABI change):**
- Declare `xi_host_api@9` frozen (ADR). Add a **CI freeze guard**: store a canonical signature (field types+order hash) per published interface; build fails if a published layout changes.
- **Golden-plugin compat test:** commit a prebuilt v6/v9 plugin DLL; CI loads + runs it every phase, asserting unchanged behavior. *Single most important safety net — do not touch the ABI without it.*

**Phase 1 — open the query door (the last-ever monolith append):**
- Append one pointer to `xi_host_api` (legal append → v10, update `XI_ABI_EXPECTED_SIZE`): `const void* (*get_interface)(const char* id, uint32_t version);`
- Host side (`make_host_api`): a `{id,version}→void*` registry; register the **entire current `xi_host_api` as `get_interface("xi.legacy", 9)`** returning the same pointer. Nothing changes for anyone; the door now exists.

**Phase 2 — carve the first interface (safest leaf as proof):** pick `xi.log` or `xi.preview` (compress) — leaf, few callers, already behind a sink. Define a frozen struct (e.g. `xi_preview_v1{ int32_t (*compress)(…); }`), register it. SDK wrapper in `xi_abi.hpp` switches to **query-then-cache with legacy fallback** (`get_interface("xi.preview",1)` else `host_->compress_image`). Capability now lives in two places during the window; new plugins use the interface, old use the field — neither breaks.

**Phase 3 — migrate domain by domain:** `xi.imaging` / `xi.doc` / `xi.emit` / `xi.log` / `xi.status` / `xi.preview`, each a frozen `@1`; one PR per domain, plugins migrate independently. Add the LV2-style handshake in `xi_cabi_adapter.hpp`'s load path (reuse the existing ABI-gate refuse-with-reason): `plugin.json` `"requires":[{"iface","min"}]` → missing required = clean refuse; optional = plugin null-checks. Use a curl-style `host_get(key, out, cap)` for scalar one-offs to avoid interface proliferation.

**Phase 4 — retire the monolith (free cleanup):** once all first-party plugins + SDK wrappers use interfaces, stop feeding new capabilities to legacy; after the deprecation window, **stop publishing `get_interface("xi.legacy",9)`**. This is "stop answering a query id," **not** a layout change → zero reshuffle, no forced sync. The dead `shm_*` stubs vanish for free (they only lived in the legacy struct, which no longer ships).

> **CAVEAT surfaced in Phase 3 (Track A2) — resolve before/within Phase 4:** the `get_interface` door is currently backed by the **canonical (default) host table** (`make_host_api`/`canonical_host_api()`), NOT the per-instance runtime-wired table. So **stateless** host services (`xi.imaging`/`xi.doc`/`xi.log`/`xi.preview`) resolve correctly, but the **`xi.emit`** pointers (`emit_record`/trigger-wired) are the canonical *default* (null until `install_trigger_hook` wires the live per-instance table). The byte-for-byte carve test holds only because both sides are null. **Before plugins consume `xi.emit@1` via the door, the door must be backed by the live wired table** (or `xi.emit` kept on the existing instance-wired path). Today no caller hits this (the only compress caller was `expose.cpp`); it bites the moment emit/trigger goes through the door.
>
> **[RESOLVED 2026-07 — commit `baa9fd1`]** The door's `xi.emit@1` is now backed by the live wired `emit_record`: `install_trigger_hook` publishes the wired fn-pointer into a process-global slot (`ImagePool::publish_emit_record`), and `emit_v1_iface()` serves a stable forwarder (`emit_record_forwarder`) that reads it — so a door caller reaches the exact same dispatch as `host->emit_record`. A debug freeze-guard (`ImagePool::door_matches_fields`, asserted in `default_host_api()` after the hook) pins every carved entry to its struct-field twin so this can't silently regress; `test_interface_domains` covers `xi.emit@1` non-null on a wired table.

**Net:** add/change/remove a capability stops being a global event; freeze lands at one-capability granularity; each plugin's dependency surface = exactly the interfaces it queried (directly serves the minimal-core goal).

## 13. Touch list (Part II)

- `STUDY-core.md` — correct ABI version (9, not 6); add full command surface + the v7/v8/v9 host services (§7).
- `backend/include/xi/xi_abi.h` — Phase 1: append `get_interface`; bump `XI_ABI_VERSION`→10, update `XI_ABI_EXPECTED_SIZE`. Later: frozen per-capability interface structs (`xi_preview_v1`, `xi_imaging_v1`, …); enum keys for `host_get`.
- `backend/include/xi/xi_abi.hpp` (or wherever `make_host_api` lives) — registry + `get_interface` thunk; register `xi.legacy@9`; per-domain registrations.
- SDK wrappers in `xi_abi.hpp` / UseProxy / `Plugin` helpers — query-then-cache with legacy fallback per capability.
- `backend/include/xi/xi_cabi_adapter.hpp` — Phase 3: LV2-style required/optional validation in the load path (reuse ABI-gate refuse).
- `backend/include/xi/xi_plugin_manager.hpp` — `plugin.json` `requires[]` parse + gate.
- CI: freeze-signature guard for published interfaces; golden-plugin load/run test.

## 14. Invariants (Part II)

1. **Composition is the script's job, one level deep** (script→plugin). Never add plugin→plugin call entry to `host_api` — that flatness is what prevents a plugin-dependency DAG.
2. **Pervasive-internal capability → ambient `host_api`; boundary-wired-once → plugin.** Do not pluginize operator/UI I/O (compress/log/status); do not keep editor services (compiler/project/UI-serve) in the kernel.
3. **`xi_core` carries the contract, never the heavy implementation** — codecs/encoders live behind a sink seam, installed by the backend, swappable.
4. **No migration step may be breaking** — every ABI move is additive or "stop answering a query id"; a published `(iface, vN)` is frozen forever (changes ship as `vN+1`). The golden-plugin test proves each step.
5. **Two evolution tracks stay separate** — hot-path capabilities via frozen ABI extensions; control-plane via WS+yyjson schema. Do not migrate control-plane into the ABI.

---
---

# Part III — Plugin Management Hardening

**Date:** 2026-07-01
**Scope:** Best-practice review of plugin **management** (discovery, load, lifecycle, isolation, crash containment) vs plugin **usage** (composition, calling, resources). Records (a) the closest mature analogs, (b) what xInsp2 already does right — *source-verified, including two items an earlier verbal pass wrongly called gaps*, (c) the three highest-leverage hardening gaps, layered.
**Status:** Analysis + plan / not yet implemented.

## 15. Framing — usage is already sound; management is where the leverage is

xInsp2's plugin **usage** model is already aligned with best practice and should not change: the host owns the dataflow graph, composition is the script's job one level deep (script→plugin, never plugin→plugin — §9, §14.1), factory/instance are split, and resources are owner-scoped and swept on death. The work to do is on the **management** side — specifically the failure-path behaviour that the in-process speed bet (§5.1) makes inherently risky.

**Closest mature analogs** (same constraints — in-process, real-time-ish, C ABI, third-party plugins):
- **Audio plugin hosts** — CLAP / VST3 / LV2: lifecycle state machine, factory/instance, activation boundary, threading contract.
- **Dataflow hosts** — GStreamer / Max-MSP / TouchDesigner / LabVIEW: host owns the patch graph, nodes are leaves.
- **Managed-plugin frameworks** — OSGi, VS Code extensions: declarative manifest, lazy activation, service registry.
- **Erlang/OTP** — supervisor trees + `code_change/3`: per-child restart budgets and state migration across code versions.

## 16. Concern map (source-verified — "already have" column is accurate)

| Concern | Best exemplar | xInsp2 today |
|---|---|---|
| Discovery & metadata | GStreamer registry cache; VS Code contribution points | ✅ **manifest-driven** — `scan_plugins` reads `plugin.json` (name/description/factory symbol/flags) without `LoadLibrary`-for-metadata (`xi_plugin_manager.hpp:6,112,135`). DLL is only loaded to instantiate. *(Earlier "don't LoadLibrary to read metadata" concern does not apply.)* |
| Lifecycle state machine | CLAP create→init→activate→process→deactivate→destroy; LV2 instantiate/activate/run | ⚠️ pieces exist (create/`prepare`/`commit` v7/process/destroy — `xi_cabi_adapter.hpp:257-270`) but **no documented state×thread contract**. The Part I bugs are the symptom (G3). |
| Factory vs instance | VST3 `IPluginFactory`; GStreamer element factory | ✅ `InstanceRegistry` + `CAbiInstanceAdapter`; config on instances, factory replayable → enables hot-reload. |
| Crash containment | DAW out-of-process scan + plugin bridge; Erlang supervisor | ⚠️ **process-granularity only** — FE `RespawnTracker` latches safe after 5 consecutive deaths, 1.5 s backoff, 30 s healthy-reset (`fe_main.cpp:63-67`); `CrashHistory` + `enrich_from_crash_report` record forensics (`fe_main.cpp:23-24`). Missing: **per-plugin attribution + disable** (G2). Scan-time load is unisolated (G1). |
| Resource ownership | RAII boundary + death sweep | ✅ owner tag + per-owner sweep (`xi_image_pool.hpp`; adapter dtor `cabi_adapter.hpp:188`); worker-thread hole being closed by Part I C1/C2. |
| Hot reload & state migration | Erlang `code_change/3`; Unreal Live Coding | ✅ schema-versioned persistent state, drop-on-mismatch; ⚠️ no explicit migrate hook (G4). |
| Capability / permission | LV2 required/optional features; WASI | ⤳ deferred to Part II's CLAP-style `get_interface` + load handshake. |
| Composition / wiring | GStreamer pipeline; Max patch | ✅ host owns graph, plugins are leaves (§14.1) — **do not change.** |
| Observability | GStreamer tracers; per-element stats | ✅ `dispatch_stats`, `image_pool_stats`, `crash_reports`/`recent_errors`; ⚠️ no per-plugin health/breaker (feeds G2). |

## 17. The three highest-leverage gaps

### G1 — scan/certification load is unisolated (a malformed plugin kills the BE at discovery)

`scan_plugins` → `register_plugin_folder` → `LoadLibraryExA(...LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)` (`xi_plugin_manager.hpp:318`) then resolves the factory exports. A third-party DLL whose `DllMain` or factory faults takes the **whole backend** down *during discovery* — and if it faults again on the respawn's scan, `RespawnTracker` latches the line safe (`fe_main.cpp:63-67`). The in-process **runtime** bet (§5.1, SHM removed 2026-05) is deliberate and stays; this is a **narrow exception for the riskiest, least-trusted moment**: first load / certification.

**Precedent:** every serious DAW scans untrusted plugins in a **child process** (Cubase/Live/Logic) so a bad scan can't kill the host; the result (ok / abi / crashed) is cached, runtime load stays in-process.

### G2 — crash budget is process-granular, not plugin-granular (highest leverage; builds on existing infra)

The supervisor already does circuit-breaking, but only for the **whole BE**: it cannot say *which plugin* killed it, so its only lever is "respawn the entire backend" → "give up and latch safe." A single bad third-party plugin therefore either crash-loops the whole line or downs it entirely. Everything needed to do better **already exists**: `CrashHistory` (structured BE-death JSONL) + `enrich_from_crash_report` give the faulting module; the codebase already has a **quarantine** concept (`.corrupt-<ts>` for unparseable `project.json` — `xi_plugin_manager.hpp:1649,2286,2409`). The missing piece is the **policy** that joins them: attribute the death to a plugin, and on the next open load the project with *that plugin disabled* rather than re-arming the same crash.

**Precedent:** Erlang/OTP supervisor — per-child restart intensity; a child that exceeds its budget is removed, siblings keep running. Hystrix/circuit-breaker — trip per dependency, not per service.

### G3 — no documented lifecycle state×thread contract (root-cause prevention for the Part I bug class)

create / `prepare` / `commit` / `process` / `set_def` / `exchange` / `destroy` each have implicit rules about *which state they're legal in* and *which thread they run on* (`prepare` runs concurrent with `process`, outside the per-instance serialization gate — `xi_abi.h:107-119`; `process` under `CallScope` unless `reentrant=true` — `cabi_adapter.hpp:100-106,244`). None of this is written down as one contract. The Part I hazards (ambient context not crossing threads) are exactly what an unwritten contract produces.

## 18. Remediation (layered)

### G1 — scan isolation
- **G1.1 (cheap, do first):** a `--certify-plugin <dir>` subcommand that loads + calls the factory once in a **throwaway child process**; parent reads exit code → records `ok|abi_mismatch|crashed` into the existing plugin record. Reuse `xi_crash_dump` so a crashed certify still yields a minidump. *No runtime path changes.*
- **G1.2:** cache certify verdicts (keyed by DLL content hash) next to the manifest; re-certify only on hash change. Folds into the existing manifest scan.
- **G1.3 (optional):** gate `scan_plugins` to skip (and surface) any plugin whose last certify verdict was `crashed`, so discovery itself can never re-arm a known-bad DLL.

### G2 — per-plugin crash attribution + auto-disable
- **G2.1 (prerequisite):** at instance `create`/`process` entry, stamp a process-global "current culprit" (instance name + plugin) — mirror `g_inspect_tid` from Part I A1. On BE death, `enrich_from_crash_report` already has the faulting thread; join it to the culprit stamp so `CrashHistory` records **which plugin**.
- **G2.2 (the policy):** a per-plugin `RespawnTracker`-style counter keyed by plugin id. N faults attributed to plugin X within the window → mark X `quarantined` (extend the existing quarantine mechanism). FE/open-project loads the project with quarantined plugins **disabled + surfaced to the operator** (`recent_errors` + a status), instead of latching the whole line safe.
- **G2.3:** an operator un-quarantine path (a WS command + UI affordance) once the plugin is fixed/rebuilt — `rebuild_plugins`/`rescan_plugins` should clear the quarantine on a content-hash change.

### G3 — write the contract
- **G3.1 (pure docs, do now):** a "Plugin lifecycle & threading contract" section (in `docs/guides/write-a-plugin.md` + a table in `STUDY-sdk.md`): for each export, the legal state(s), the thread it runs on, the serialization guarantee (`CallScope` vs `prepare` concurrency vs `reentrant`), and the ambient-context rule (Part I Invariants). One table; the single best defense against re-introducing the Part I bug class.
- **G3.2 (optional, later):** a debug-build state assertion in `CAbiInstanceAdapter` that traps illegal transitions (e.g. `process` before `commit`, `set_def` during `process` on a non-reentrant instance).

### G4 — lower priority
- Explicit `code_change`-style state-migration hook (an optional plugin export the host calls on schema-version change) instead of drop-on-mismatch — only if cross-version state continuity becomes a requirement.
- Capability required/optional handshake — folds into Part II Phase 3; do not build separately.

## 19. Touch list (Part III)

- New: a `certify-plugin` child-process entry (likely a mode of `runner_main.cpp` or a small sidecar) — G1.1; verdict cache next to manifest — G1.2.
- `backend/include/xi/xi_plugin_manager.hpp` — G1.3 certify gate in `scan_plugins`; G2.2 plugin-keyed quarantine (extend the existing `.corrupt-<ts>` mechanism) + un-quarantine on hash change (G2.3).
- `backend/src/service_main.cpp` / `xi_cabi_adapter.hpp` — G2.1 culprit stamp at `create`/`process` entry (mirror Part I A1's `g_inspect_tid`).
- `backend/src/fe_main.cpp` + `xi_respawn_policy.hpp` / `xi_crash_history.hpp` — G2.2 per-plugin counter + load-with-disabled policy; join culprit into `enrich_from_crash_report`.
- WS protocol — G2.3 un-quarantine command + quarantined-plugin status surface.
- `docs/guides/write-a-plugin.md`, `docs/reference/STUDY-sdk.md` — G3.1 lifecycle×thread contract table.
- `xi_cabi_adapter.hpp` — G3.2 (optional) debug illegal-transition asserts.

## 20. Invariants (Part III)

1. **The in-process runtime bet stays** (§5.1). Process isolation returns *only* for scan/certification (the least-trusted moment), never the dispatch hot path.
2. **A crash is attributed to a plugin before it is acted on.** The supervisor's lever graduates from "respawn/latch the whole BE" to "disable the offending plugin, keep the line up."
3. **Quarantine is one mechanism.** Reuse the existing `.corrupt-<ts>` concept for crash-prone plugins; do not invent a second disable path. Un-quarantine is always operator-visible and hash-gated.
4. **The lifecycle×thread contract is written down before it is enforced** — docs (G3.1) precede asserts (G3.2). An unwritten contract is what produced the Part I bug class.
5. **Usage model is frozen** — host owns the graph, composition stays script-level one-deep. None of this hardening adds a plugin→plugin path.

---
---

# Part IV — Correctness Tooling & Roadmap Sequencing

**Date:** 2026-07-01
**Scope:** A best-practice audit of dimensions not covered by Parts I–III (overload/backpressure, allocator, testing/fuzzing, perf gates, trust, protocol/schema contracts, observability), source-verified; and the **sequencing rule** for the whole roadmap — what must precede the structural work in Parts II–III.
**Status:** Analysis + plan / not yet implemented.

## 21. Verified scorecard (source-checked 2026-07-01) — what is already handled vs a real gap

A verbal first pass over-stated several gaps. Verified against source and the unmerged fuzz branches:

| Dimension | Verdict | Evidence |
|---|---|---|
| **Backpressure / overload** | ✅ **handled** — *not a gap* | Per-lane **bounded** queue `cfg.queue_depth` (`service_main.cpp:1473,1479`); configurable **drop_oldest / drop_newest** with **observable** `XI_SYS_DROPPED=-999001` markers (`:681,1498,1510`); `min_interval` rate-limit + coalesce "latest wins" (`:1362,1598`); per-lane high-watermark (`:1342`); explicit pool-exhaustion (`xi_image_pool.hpp:140`) + 1 GiB cap. Drop-based (not block-based) is the correct choice for cameras — aligned with CoDel/latest-wins. |
| **Error taxonomy** | ✅ handled | `xi_result.hpp` — signed code, framework system-band ≤ -990000 reserved, one Result/run. |
| **Config validation** | ✅ handled | `validate_config_against_manifest` (`xi_config_validate.hpp`); runtime required-field checks (`xi_record.hpp:793`). |
| **Black-box fuzz of WS/config/emit/compile** | ⚠️ **done once, decayed** | `origin/feature/fl-r7-fuzz`, `origin/feature/fl-r8-concurrency-fuzz` — thorough Python harnesses; findings ticketed (`fix/r7-p0-reader-disconnect`, `fix/r7-p1-ws-single-client`). **But** one-shot surveys under `examples/`, unmerged, non-gated; **~half target the removed process-isolation IPC layer** (see §25). |
| **In-process coverage-guided fuzz** (libFuzzer/AFL on `parse_cmd`/yyjson/ABI `process()`) | ❌ **real gap** | No such target anywhere. Black-box-over-WS cannot reach the coverage-guided in-proc paths. |
| **TSan / ASan / UBSan in build** | ❌ real gap | Not wired (only yyjson vendor self-tags `no_sanitize`). Concurrency tests (`race_probe`, `test_set_def_race`) are **probabilistic**, not systematic. |
| **Perf-regression gate** | ❌ real gap | `bench_*` exist but `CMakeLists.txt:230` — *"not a ctest — run manually."* No baseline, no threshold. |
| **Observability export** | ⚠️ partial gap | `graph_capture` = dataflow **topology only** (`xi_graph_capture.hpp:3`); `dispatch_stats`/`image_pool_stats` are point queries. No metrics/tracing export, no latency histogram/p99. |
| **WS protocol versioning** | ⚠️ partial gap | `cmd:version` reports `XINSP2_VERSION` (`service_main.cpp:1137,1926`) but no negotiation / min-client gate. |
| **Record cross-plugin schema contract** | ⚠️ partial gap | Runtime missing-field checks only; no producer→consumer static contract (tension with the schema-less design). |
| **Plugin trust / signing** | 🔵 deliberate non-goal | "Plugins are trusted (no baseline cert gate — removed 2026-06)" (`xi_plugin_manager.hpp:1150`). `sha256` is JPEG-cache only. Revisit *with* Part III G1, not standalone. |

**Net:** the genuine, non-deliberate gaps are **(a) in-process coverage-guided fuzz, (b) sanitizers in the build, (c) a perf-regression gate** — plus the decayed r7/r8 harnesses to salvage. Backpressure, error taxonomy, and config validation are already sound; observability/protocol/schema-contract are partial and lower priority.

## 22. The sequencing rule — safety net before structural change

Part II §12 already encodes this for the ABI: **Phase 0 (golden-plugin + freeze guard) must precede any ABI change.** Part IV generalizes the same rule to the rest of the roadmap. The justification is the §5.1 speed-over-isolation bet: a concurrency/ABI regression does not return an error — it **crashes the whole backend** (production line down, FE respawn). The cost of catching such a regression *late* dwarfs the bounded cost of the tooling. Therefore the generic safety nets are not "nice to have later" — they are the precondition for touching the dispatch/ABI/plugin surfaces in Parts II–III.

**But not as a monolith.** The dependency is specific: the net that guards a change precedes *that* change, not all tooling before all features. Split accordingly.

## 23. Tier 0 — generic nets (do BEFORE the structural work in Parts II–III)

Cheap, broadly protective; each protects every subsequent change.
- **T0.1 — sanitizers in the build:** a TSan config (and an ASan/UBSan config) of the test suite. TSan turns the probabilistic `race_probe`/`set_def_race` into systematic detection — the single highest-ROI net for an in-process concurrent core.
- **T0.2 — salvage + gate the r7/r8 harnesses** (§25): drop the obsolete process-isolation targets, keep WS-cmd / config / emit / compile-concurrency, promote from one-shot `examples/` survey to a maintained smoke run (a reduced-iteration `FUZZ_ITERS` mode wired into the test/CI flow).
- **T0.3 — perf-regression baseline:** promote `bench_image_pool` / `bench_jpeg` / `bench_record` to ctest with a recorded baseline + a regression threshold that fails the build. Guards the speed bet directly.
- **T0.4 — already done:** golden-plugin compat test + ABI freeze guard (Part II Phase 0 / W2); Part I hazard regression tests (in progress).

## 24. Tier 1 — targeted tooling (rides WITH the work it guards, not before everything)

- **T1.1 — in-process libFuzzer on the parser boundary:** `parse_cmd` (`xi_protocol.hpp`) + the yyjson decode path. Can land in Tier 0 if cheap, but is *mandatory alongside* Part II Phase 1 when `get_interface(id, version)` adds a new in-proc parsing surface.
- **T1.2 — libFuzzer on the ABI `process()` input** (record/doc decode): schedule **with** Part II's per-capability interface carving.
- **T1.3 — TSan stress on per-plugin crash attribution + quarantine:** schedule **with** Part III G2 (the new concurrent culprit-stamp + counter paths).

## 25. r7/r8 salvage list

`origin/feature/fl-r7-fuzz` (`0c7b32a`) + `origin/feature/fl-r8-concurrency-fuzz` (`8cb5976`) — neither merged.

**Keep (still-valid surfaces, were healthy when surveyed):**
- r7 #1 `harness_ws_cmd.py` — WS cmd JSON parser (1500+ iters, 0 crashes; one P1 accept-stall, ticketed `fix/r7-p1-ws-single-client` — confirm it landed).
- r7 #2 `harness_config.py` — manifest/instance/project validation (0 findings).
- r7 #4 `harness_emit_trigger.py` — emit_trigger/RPC in-proc path (0 findings).
- r8 `harness_emit_x_cmd.py`, `harness_cmd_during_compile.py`, `harness_set_param_storm.py` — emit×cmd contention, cmd-during-compile, set_param storm (all healthy).

**Drop (target the removed 2026-05 process-isolation layer — code no longer exists):**
- r7 #3 `harness_evil_worker_host.py` + `evil_worker.cpp` + the `evil_worker` CMake target (IPC frame parser / `ProcessInstanceAdapter` / `xinsp-worker.exe`). Its headline P0 was real then but is moot now; `fix/r7-p0-reader-disconnect` addressed it in-era.
- r8 `harness_backend_kill.py`, `harness_open_close_cycle.py` worker-cleanup portions (worker-process lifecycle).

**Note in any revival doc:** the FRICTION_FUZZ.md "CRITICAL" verdict is historical — do not let it read as a live finding against the current single-process core.

## 26. Touch list (Part IV)

- `backend/CMakeLists.txt` — T0.1 sanitizer build configs; T0.3 promote `bench_*` to ctest + baseline/threshold; T0.2 wire a smoke target.
- New `examples/fuzz/` (or `backend/tests/fuzz/`) — T0.2 salvaged r7/r8 harnesses (kept subset, §25), reduced-iteration smoke mode; T1.1/T1.2 libFuzzer targets on `parse_cmd` / yyjson / ABI `process()`.
- CI (when one exists) — run TSan suite + fuzz smoke + perf-gate; until then they run in the local test flow (same substitute the freeze guard uses).
- `STUDY-core.md` / `STUDY-sdk.md` — record that backpressure/error-taxonomy/config-validation are verified-sound (so they aren't re-chased).

## 27. Invariants (Part IV)

1. **A safety net that guards a surface precedes the change to that surface** (generalizes Part II Phase 0). Tier 0 before Parts II–III; Tier 1 rides with its feature.
2. **Backpressure is a solved problem here** — bounded lanes + observable drop policy. Do not "fix" it; do not switch cameras to block-based.
3. **Fuzz findings carry their era** — a survey against removed code is not a live finding. Prune obsolete targets when salvaging; never let a stale "CRITICAL" gate present work.
4. **Tooling failures fail the build** — sanitizer races, fuzz crashes, and perf regressions are build-breaking, not advisory (the repo's CI-less substitute, same as the freeze guard).
5. **Don't gold-plate the partial gaps** — observability export, WS protocol negotiation, and Record schema contracts are real but lower priority than the three correctness nets; sequence them after, only if a concrete need appears.

## 28. Data-race test surface (backs OQ-1 / T1.3 — recorded 2026-07-01, decide later)

Races live in the **shared mutable state that multiple dispatch workers touch concurrently** (the cost of the §5.1 speed-over-isolation bet). "Test data race" concretely = run the concurrency tests below **under TSan** (probabilistic → systematic). TSan is **Linux-only** here (OQ-1); Windows ASan + `XINSP2_STRESS_SCALE` catches only the *memory-corruption subset* (UAF/double-free) a race produces, not a pure unsynchronized read/write.

### Tier 1 — shared refcounted pools + dispatch (highest value)
| Hotspot | Shared state / race | Existing test |
|---|---|---|
| **Image pool** | lock-free lookup + mutex-guarded create/release + per-entry refcount atomics + generation/ABA; `release_all_for(owner)` sweep vs concurrent create | `test_image_pool_stress` (+ `image_pool_stress_heavy`) |
| **DocRegistry + Record COW** | 16-shard refcount map; cross-ABI shared-doc `retain`/`release`/`share_out`/`adopt_shared` balance; frozen-doc first-mutation COW (fan-out read + one mutate) | `test_doc_registry`, `test_record`, `test_doc_pool` |
| **Dispatch queue + EmitGate** | GroupLane deque enqueue(source) vs dequeue(worker) + drop policy (drop_oldest/newest) + high-watermark atomic; EmitGate `seq_next` CAS + emission-cursor advance (ordered emit) | ✅ `test_emit_gate` (+ `emit_gate_heavy`) — ordered-emit + dtor backstop + stop-wakes-waiters. *(GroupLane deque/drop-policy itself is still black-box via the r8 surge tests; EmitGate primitive is now dedicated.)* |

### Tier 2 — lifecycle / config concurrency
| Hotspot | Race | Existing test |
|---|---|---|
| **set_def vs process** (CallScope) | non-reentrant instance config "tear" — process reads half-written config | `test_set_def_race` + `race_probe` (+ `set_def_race_heavy`) |
| **prepare vs process** | prepare runs OUTSIDE the gate, concurrent with process, mutating instance state while process reads | `test_prepare_concurrency` |
| **Hot-reload g_script swap** | `g_script_mu` swap; in-flight inspect snapshots the shared_ptr; deferred FreeLibrary | ✅ `test_hot_reload_swap` (+ `_heavy`) — real loader, snapshot-by-value readers vs reload writer, distinct on-disk copies so FreeLibrary truly unmaps |
| **param/instance cache** | set_param vs a run reading params (10 kHz storm) | r8 `set_param_storm` (black-box only) |

### Tier 3 — newer / ambient
| Hotspot | Race | Existing test |
|---|---|---|
| **G2 culprit stamp** | `g_culprit` (process-global) stamped at boundary, read by crash handler on the faulting thread | `qa_quarantine_heavy` (ASan/stress) |
| **C1/C2 owner propagation** | owner get/set across async/parallel_for; OwnerScope install/restore | ✅ `test_owner_cancel_stress` (+ `_heavy`) — 6 concurrent owner epochs hammering async+parallel_for; exact per-owner attribution + sweep |
| **watchdog cancel flag** | watchdog thread sets global cancel; inspect polls it | ✅ `test_owner_cancel_stress` — epoch semantics (in-flight cancelled / post-trip spared) + arm/clear-vs-begin/poll contention (ticket-counter integrity) |
| **status registry** | `set_status` coalesce map written by many plugin threads | ⚠️ GAP (deliberate): `static` mutex-guarded `std::map` in `service_main.cpp` (`set_status_internal`), not header-isolatable; already the solved lock pattern. Covered black-box by `qa_*` WS tests; not gold-plated with an exe-linked harness. |

### Two actions (sequence later)
1. **Windows-doable now — ✅ DONE (2026-07-01):** filled the Tier-1/2/3 gaps — `test_emit_gate` (dispatch/EmitGate ordered-emit), `test_hot_reload_swap` (g_script swap), and `test_owner_cancel_stress` (owner propagation + watchdog epoch-cancel), each with an `XINSP2_STRESS_SCALE` `_heavy` variant. Catches the corruption subset today. Only remaining sub-gap: the GroupLane deque/drop-policy itself (black-box via r8 surge) and the status coalesce map (mutex-guarded, deliberately black-box — see Tier-3 note).
2. **Needs OQ-1 decision:** stand up a **Linux CI lane** and run all the above concurrency tests under **TSan** — the only way to catch pure data races (unsynchronized read/write without corruption). Same lane also unlocks real UBSan/libFuzzer at scale.
