# xInsp2 Deep Architecture Review — 2026-04-28

Companion to `architecture-review-2026-04-28.md`. None of the issues there
(process isolation default, frame_path phantom, ad-hoc JSON parsers in
project_locked, save_project_locked escaping, VAR redeclaration trap,
single-client globals, in-proc crash class) are repeated. Cited
file:line references throughout.

---

## 1. `xi::Record` + cJSON memory model

The Record contract is `unique_ptr`-shaped — the cJSON tree is
exclusively owned, freed in the destructor at
`backend/include/xi/xi_record.hpp:58`. Copy ctor/op (`:62`/`:66`) deep-
duplicate via `cJSON_Duplicate(..., true)`; move ctor/op
(`:75`/`:79`) transfer the pointer and null the source. So same-DLL
ownership is sound.

The cross-DLL story is where the model leaks.

**Finding 1.1 — `xi_record_out` allocates with `_strdup`/`malloc` in
the plugin's CRT, frees in the host's CRT.**
`xi_record_out_init/_set_json/_add_image/_free` are `static inline` in
`backend/include/xi/xi_abi.h:176-216`, so each translation unit gets
its own copy. The plugin DLL's `xi_plugin_process` calls
`record_to_c` which calls `xi_record_out_set_json` (`xi_abi.hpp:238`)
— that `_strdup`s into the plugin's malloc heap. The host then calls
`xi_record_out_free` (its inline copy, in the backend CRT) which
`free()`s `out->json` and the per-image `entry->key` strings. Mixed
CRTs (and even same-CRT-but-different-DLLs with the `/MD` runtime can
share a heap, but only if both link the *same* `vcruntime140.dll`
version — which is exactly what we're trying not to assume in a
plugin loader).

- *Worst case*: heap corruption on every single `process()` call when
  a plugin is built against a vcruntime that differs from the
  backend's. Manifests as random crashes, often deferred until much
  later, on plugin shutdown or under load.
- *Severity*: high. This is the textbook DLL ABI mistake that
  `xi_image_handle`-via-host-API was designed to avoid; we then
  reintroduced it for `out->json` and `out->images[i].key`.
- *Repro*: build a plugin with `/MT` while backend is `/MD`, run any
  process call, watch for `_CrtIsValidHeapPointer` asserts under
  debug CRT.
- *Fix sketch*: route `xi_record_out` allocation through `xi_host_api`
  (a `host->json_alloc` / `host->record_image_add`) the same way
  image handles do, so all malloc/free happens in one CRT. Or change
  the contract so the plugin returns a `const char*` it owns and the
  host copies into its own buffer before free, with the plugin's free
  function called via the DLL.

**Finding 1.2 — `record_from_c` silently flattens JSON.** At
`xi_abi.hpp:209-216` the helper iterates only top-level keys and uses
`set(key, valuedouble)` for *every* number. cJSON stores both
`valueint` (truncated to `int`) and `valuedouble`; the host-side
`set(double)` writes a number node, but that means a JSON `int64`
that happens to be `> 2^53` arrives in the plugin as a lossy double.
For nested objects/arrays the code calls `set_raw(...,
cJSON_Duplicate(item, true))` which preserves the subtree — so the
flattening is *only* for top-level scalars. This is asymmetric: a
top-level `{"id": 9007199254740993}` round-trips wrong; the same
field under one nesting level round-trips fine. Severity: medium,
silent. Fix: walk into all top-level types via `cJSON_Duplicate` for
non-string-non-bool too, and stop using `valuedouble` for ints.

**Finding 1.3 — Record default ctor cannot fail-soft.**
`Record() : json_(cJSON_CreateObject())` (`xi_record.hpp:56`). If
cJSON's malloc fails, `json_` is null and every subsequent `set()`
calls `cJSON_DeleteItemFromObject(json_, ...)` — cJSON tolerates that
— and `cJSON_AddNumberToObject(json_, ...)` which silently does
nothing because the parent is null. The Record then serializes as
`{}` and the plugin author has no way to know data was dropped.
Severity: low (OOM only) but contract-violating. Fix: either fail
loudly (throw `bad_alloc` from the ctor) or make every `set` lazy-
construct.

## 2. `xi::async` / `Future<T>`

**Finding 2.1 — Cancellation is not implemented; "stuck Future
prevents script reload" is real.** `Future<T>` (`xi_async.hpp:32`)
wraps `std::future<T>` from `std::async(std::launch::async, ...)`
(`:113`). There is no cancel(), no token, no detach. If the
inspect loop spawns `xi::async(slow_op)` and the slow op blocks
forever on I/O, the inspect thread is stuck in `Future::operator T`
→ `f_.get()`. The watchdog at `service_main.cpp:2492` will
`TerminateThread` the inspect thread — but **not** the std::async
worker thread holding the future's shared state. The shared state
contains a heap-allocated `std::promise` whose destructor blocks
until the worker thread completes (`std::async` destructor
semantics). So when `Future` is destructed at scope exit, the dtor
*joins the runaway worker*. After watchdog termination of inspect,
the next compile-and-load still works (g_script swap holds
g_script_mu only), but any pending Future objects in the killed
thread's stack frames are now leaked + their workers leaked
permanently.

- *Worst case*: every watchdog trip leaks one std::async thread per
  outstanding Future. Over time the process accumulates zombie
  workers each consuming a stack and a kernel thread.
- *Severity*: medium (slow leak, requires repeated trips).
- *Fix sketch*: introduce a cancellation token threaded through
  `xi::async` and consulted by all wrapped ops; or detach the
  std::future and accept the leak explicitly with a counter.

**Finding 2.2 — `Future::get()` returns by value via `T cached_{}` and
copies on every implicit-conversion.** `:50-53`. For `Future<Image>`
each `operator T` invocation returns a *copy* of the cached Image.
Image probably has a shared-pixel buffer (cheap), but Record
(potentially returned by an async step) does a full `cJSON_Duplicate`
on every read. Plus `T cached_{}` requires `T` default-constructible.
Severity: low/perf. Fix: cache as `std::optional<T>` and return
`const T&` from `get()`; provide explicit `T take()` for r-value.

**Finding 2.3 — TOCTOU in `ready()`.** `:55-59`. Reads `consumed_`
non-atomically then queries `wait_for`. If two threads observe a
not-yet-consumed Future, both see `false` for `consumed_`, both fall
into `wait_for`, fine — but the class is documented "Safe to call
multiple times" yet is *not* actually thread-safe: copy ctor is
deleted, but two threads with the same reference racing `get()`
would race on `cached_ = f_.get()` (one would see `consumed_ = true`
without a memory fence and read garbage `cached_`). Severity: low
(the API surface discourages this) but the comment is misleading.
Fix: drop the "safe to call multiple times" claim or add a mutex.

## 3. TriggerBus correlation under load

**Finding 3.1 — `id_key(tid) = hi ^ lo` collapses 128 bits to 64
with trivial collisions.** `xi_trigger_bus.hpp:290`. Two distinct
random 128-bit ids whose 64-bit XOR happens to match share a slot
in `pending_` and `_locked` lookups. Worse: `make_trigger_id`
(`:67`) explicitly seeds `lo = 1` if both halves are zero, but a
caller that supplies its own `tid` (camera plugin recovering an
external sync ID) can pass `{hi=X, lo=X}` for which `id_key == 0`,
which is the same id_key as `{0,0}`. AllRequired then merges
unrelated emits.

- *Worst case*: under AllRequired with an external trigger source,
  two events get cross-correlated; the worker dispatches a
  TriggerEvent containing images from two different physical
  acquisitions. Inspection result is wrong but no error is raised.
- *Severity*: high — wrong inspection output is the worst possible
  failure mode for an inspection system.
- *Repro*: AllRequired with two sources, supply `{hi=0xAA, lo=0xAA}`
  and `{hi=0xBB, lo=0xBB}` in succession.
- *Fix*: key by the full 128-bit value (use a struct with hash/eq
  combining hi and lo, or a `std::string` of 16 bytes).

**Finding 3.2 — AllRequired drops on policy change AND drops on slow
source.** `:131-135` plus `evict_stale_locked` at `:299-312`. When a
fast camera + slow camera both emit on the same tid, the slow one
arrives just past `window_ms_`. The fast frame is `release()`d at
`:305` — but the slow camera's *next* emit then creates a fresh
pending entry for the same tid (well, same `id_key` per 3.1) and
becomes the new "first_seen". If it later completes, the dispatched
event has only the slow source's image plus whatever followers
emitted *afterwards*, never the fast source's original frame. Also,
`evict_stale_locked` runs only on *new* emits — if the source goes
silent, evicted-pending images are not released until the next emit
or until policy is changed. A long pause ⇒ image handle backlog held
indefinitely.

- *Worst case*: under-provisioned camera produces a steady leak of
  image-pool refs that never release; eventually pool exhausts.
- *Severity*: medium.
- *Fix*: run the evict pass on a timer, not on emit; and detect
  duplicate-source-replacement-after-evict to log a "frame dropped"
  warning.

**Finding 3.3 — LeaderFollowers caches followers without bound.**
`:226-235`. `follower_latest_` is keyed by `<source>` or
`<source>/<key>`. A misbehaving follower that publishes new keys
every frame (e.g. timestamp-suffixed) grows `follower_latest_`
unboundedly. Each old entry holds an image-pool ref. `reset()`
(`:273`) is the only cleanup, called on script reload. Fix: cap
followers to the explicit list registered by the project; reject
unknown follower names.

**Finding 3.4 — Leader emits before any follower attaches → silent
"empty" event.** `:210-224`. If the leader emits first,
`follower_latest_` is empty, the dispatched event contains only the
leader image. The script has no way to distinguish "follower not yet
attached" from "follower stopped". For a synchronized-camera setup
this means the first N inspections silently skip a camera. Fix:
optionally hold leader emits until at least one frame from each
configured follower has been seen, behind a `wait_for_followers`
project setting.

## 4. Hot reload state preservation edge cases

`service_main.cpp:950-1014` is the load path; state goes through
`get_state` → `g_persistent_state_json` → `set_state`.

**Finding 4.1 — Buffer growth-and-retry on `get_state` is not
loop-bounded.** `:954-957`. Initial buffer 256 KiB. If `n < 0`,
resize to `(-n) + 1024`, retry once. If between the two calls the
state grew, second call also returns negative — the code then
silently uses an empty `g_persistent_state_json`. In practice
inspect() is paused during the swap (g_script_mu held), so it can't
grow, but the implementation contract is fragile. Severity: low.
Fix: loop until non-negative or write a one-shot `int
xi_script_get_state_size()` helper.

**Finding 4.2 — Schema migration is impossible by construction.**
`xi_script_set_state` (`xi_script_support.hpp:304-322`) parses the
JSON and rebuilds `xi::state()` key-by-key. The new DLL has no
opportunity to run a migration before set_state — and after set_state
the script gets the data raw. If the old DLL had `state["roi"]` as
an object and the new DLL expects a string, the new DLL just sees a
mismatched type via `state["roi"].as_string("")` and silently gets
the default. No diagnostic, no log line, no version field.

- *Worst case*: a working project, after a script edit, silently runs
  with default state and produces wrong inspection results.
- *Severity*: medium.
- *Fix*: include a `__schema_version` field that the script is
  expected to bump and a service-side hook for the new DLL to
  request migration.

**Finding 4.3 — `xi::use<T>` template type change reattaches to a
stale instance.** `service_main.cpp:986-993` rewires
`set_use_callbacks` after the new DLL loads, but the plugin
instances themselves persist across script reload (they're owned by
PluginManager, not the script). If the script changed
`auto& cam = xi::use<MockCamera>("cam0");` to
`xi::use<RealCamera>("cam0")`, the underlying instance is whatever
the project.json says it is. The new template type is not validated
against the actual plugin's manifest. The script reads/writes via
exchange_instance JSON, so wrong types are caught at runtime by the
plugin's exchange parser at best, silently no-op'd at worst.
Severity: low (defensive plugins are fine) but the template type is
purely a façade. Fix: add a manifest type-tag check during the wire-
up.

**Finding 4.4 — Param range change does not clamp persisted value.**
`xi_param.hpp:121-127`: `set` clamps to range. But `set_state` does
not flow through ParamRegistry — Params are reconstructed by the new
DLL with their *new* defaults, then `set_param` is called from
project.json by `service_main.cpp:1503-1517`. The
`g_script.set_param` route hits the new DLL, which routes through
`Param::set_from_json` → `Param::set` → range clamp. So the value
*is* clamped on apply. Good. **However**, the project.json on disk
still holds the unclamped old value, and a subsequent
`save_project` will overwrite it with the clamped one — fine — but
between load and next save, a crash leaves the project.json with an
out-of-range value forever. Severity: very low. Fix: clamp+save once
on load.

## 5. Plugin DLL lifecycle

**Finding 5.1 — Constructor that allocates host-pool images then
throws leaks them.** `XI_PLUGIN_IMPL`'s `xi_plugin_create`
(`xi_abi.hpp:257-260`) does `try { return new ClassName(host, name);
} catch (...) { return nullptr; }`. The C++ object's destructor does
not run on construction failure; any host-allocated `xi_image_handle`
the constructor obtained via `host->image_create` is leaked into the
backend's image pool with no plugin pointer to release it. Severity:
medium (only on construction failure, but cumulative across retries).
Fix: pattern is to use RAII `HostImage` from `xi_plugin_handle.hpp`
inside the constructor — but that's not enforceable. The macro
should at minimum log the throw text.

**Finding 5.2 — Destructor that throws crosses FreeLibrary boundary
unsafely.** `CAbiInstanceAdapter::~CAbiInstanceAdapter`
(`xi_plugin_manager.hpp:101-103`) calls `destroy_fn_(inst_)` which
is the plugin's `xi_plugin_destroy`:
`delete static_cast<ClassName*>(inst);` (`xi_abi.hpp:262-265`). If
the destructor throws, the exception unwinds through C-ABI, which
is undefined behavior and on MSVC simply terminates. Worse, after
that destroy, `FreeLibrary` is called by `~PluginManager` (`:203-
208`). If a plugin registers static destructors that touch the
backend's `ImagePool::instance()` after FreeLibrary's DllMain
DLL_PROCESS_DETACH path, the static state might still be valid (it
lives in the backend), but if the plugin author was clever enough
to keep a singleton-Map<HostImage> in their DLL, those HostImage
dtors call into the host's `image_release` after the host has
already torn down. Severity: medium, exotic. Fix: document "no
non-trivial statics across FreeLibrary"; consider blocking
FreeLibrary on shutdown entirely (let the OS handle it).

**Finding 5.3 — `recompile_project_plugin` does not run cert.**
`xi_plugin_manager.hpp:521-552`. The compile-failure path
re-instantiates against the *old* DLL (good), but the success path
just LoadLibrary+GetProcAddress and goes — no cert::is_valid check
the way `load_plugin` does at `:774`. Comment at `:362-365` says
"Project plugins skip the cert/baseline gate". Fine for
project-local code, but a developer who runs the inspection
continuously while editing relies on baseline tests as a regression
gate. The recompile path silently runs uncertified code. Severity:
low (by-design) but worth a one-line warning: "uncertified plugin
loaded".

## 6. Recording / replay semantics

**Finding 6.1 — Replay tid collides with live emits.**
`xi_trigger_recorder.hpp:304`. Replay calls
`TriggerBus::instance().emit(source, e.id, ...)` with the
*recorded* tid. If a live source happens to be running concurrently
and emits its own tid (random 128-bit), under id_key collision (see
3.1) those collide. Even without id_key collision: if a recording
contains tid X and the current PRNG independently rolls tid X (1 in
2^128, negligible) — fine. But the test recording could deliberately
have predictable tids (e.g. `{hi=0, lo=N}`) which AllRequired then
merges with new live emits. Severity: medium (test/debug scenarios).
Fix: `start_replay` should `clear_observer` and `pause` live emits
or rewrite tids on the fly.

**Finding 6.2 — Frame ordering is not preserved across multi-source
recordings.** `:298` iterates `by_source` (a `std::unordered_map`)
to emit. Iteration order is unspecified — for a single tid with
sources A and B, replay run #1 may emit A then B, run #2 may emit
B then A. Under AllRequired both work; under LeaderFollowers the
order matters: if leader emits first, no followers are cached →
empty followers in event (3.4). So whether the replay reproduces
the original behavior is non-deterministic. Severity: medium for a
"replay" semantic that is supposed to be reproducible.
Fix: emit in the order the recording manifest stored, or at minimum
deterministic-sort by source name.

**Finding 6.3 — `manifest.json` parser is one-line-per-event.**
`load_manifest` at `:208` requires the entire event JSON on a single
line. The writer at `write_manifest` `:182-198` produces that form
exactly, but it's brittle: any external pretty-printer or git merge
that wraps a long `frames` block breaks parsing silently (the line
is skipped, replay reports zero events). Severity: low. Fix: use a
proper JSON parser (cJSON is already linked).

**Finding 6.4 — `events_` accumulates unboundedly during long
recording.** `:171` push_back forever. A recording that runs for
hours can OOM. Severity: low. Fix: optional `max_events` cap with
ring-eviction or rotate-file strategy.

## 7. Watchdog + `cmd:run` thread interaction

The inline comment at `service_main.cpp:2477` ("Resources leak
(TerminateThread is unsafe by design)") understates which.

**Finding 7.1 — Locks held by the killed thread are leaked.**
`run_one_inspection` at `:530-540` acquires `g_script_mu` (line
~538 inside the function — the script is copied while holding the
lock). The lock is *released* when the std::lock_guard goes out of
scope. If TerminateThread fires *during* `s.inspect(frame_hint)`
(`:573`) the lock_guard has already gone out of scope (it was
scoped to the load read-block), so g_script_mu is NOT held at the
moment of termination. Good.

But ImagePool — `xi_image_pool.hpp` — uses sharded mutexes (per the
comment in `TriggerBus::emit` at `:150`). If the inspect thread is
mid-`ImagePool::release()` holding a shard mutex when terminated,
that shard is locked forever. The next plugin-side
`addref`/`release` for that shard hangs the entire backend.
Severity: high — single watchdog trip can wedge all later
inspections. Fix: the only safe shape is process isolation
(already in flight per the user note); short-term, switch
ImagePool's shards to `std::recursive_timed_mutex` and bail out on
timeout.

**Finding 7.2 — Mid-allocation kill leaks heap blocks AND can
corrupt CRT.** TerminateThread doesn't just leak — if the killed
thread is inside the CRT heap lock (any malloc/new), the heap lock
is permanently held. MSVC's heap recovers in some cases (HeapLock
is per-handle, terminating a thread holding it deadlocks future
HeapAlloc on that heap). Severity: high. Fix: same — process
isolation. Document explicitly that watchdog trip should be
followed by a process restart.

**Finding 7.3 — Disarm race.** `:566-570` defines `disarm` as a
lambda. The watchdog at `:2487` does `g_inspect_thread_handle
.exchange(nullptr)` and `g_inspect_deadline_ms.store(0)` between
the inspect thread reaching its post-inspect point and calling
`disarm()`. If the watchdog wins the exchange, both the watchdog
and the (still-running) inspect thread end up calling `CloseHandle`
on the same HANDLE — `disarm` reads `g_inspect_thread_handle
.exchange(nullptr)` getting nullptr (watchdog already took it), so
no double-close. OK. **However** the watchdog at `:2492` calls
TerminateThread on a thread that is already past `s.inspect(...)`
— it will TerminateThread a thread that is happily emitting var
frames or returning. Severity: medium (kills a non-runaway thread
just because the timer expired before the disarm landed). Repro:
inspect that takes exactly `wd_ms - 50ms` of work + 60ms of
post-processing. Fix: disarm BEFORE post-processing (move
`disarm()` to immediately after `s.inspect`), which the code at
`:574` already does. Re-check: yes, disarm is called at `:574`,
`:576`, etc.; race window is the gap between the deadline check at
`:2486` and the `exchange` at `:2487`. Watchdog can still terminate
a thread that has already passed `s.inspect` but not yet hit
`disarm`. Mitigation: have watchdog re-check deadline *after*
exchange and bail if zero.

## 8. Per-plugin baseline cert

**Finding 8.1 — Cert "tamper detection" relies on file size + mtime;
both are trivially forgeable.** `xi_cert.hpp:107-116`. Any process
that swaps the DLL while preserving `dll_size` (pad to a known
size) and `dll_mtime` (`SetFileTime`) bypasses cert with no
re-baseline. Severity: medium for a cert system. Note: collision
resistance is irrelevant per scope; the question is tamper
detection, and `(size, mtime)` is not a digest. Fix: store
SHA-256 of the DLL bytes; the `BCryptHashData` Win32 path is one
function call, no new dep. The atomic_write infrastructure already
exists.

**Finding 8.2 — Stale cert produces silent re-cert on load.**
`xi_plugin_manager.hpp:774-796`. If `is_valid` returns false the
plugin is re-baselined inline during `load_plugin`. Baseline tests
take ~hundreds of ms (per the comment at `:780`). For a workflow
where the developer rebuilds a plugin and reopens the project, this
is a UI-blocking load with no progress indication. Severity: low,
UX. Fix: emit a `LogMsg` "certifying plugin X..." before the
`baseline::run_all` call.

**Finding 8.3 — Cert failure error to user is fprintf-only.**
`xi_plugin_manager.hpp:783-790` writes to stderr; `load_plugin`
returns false; the calling site converts that to "factory not
found" or similar generic message. The user sees "plugin not
loaded" with no indication that *cert failed*. Severity: medium.
Fix: thread the Summary up through `last_open_warnings_` so the
extension can show "plugin X failed baseline test 'process_empty'".

## 9. Remote backend auth

**Finding 9.1 — Handshake itself is trivially replayable.**
`xi_ws_server.hpp:407-441`. Auth is a static bearer token in the
HTTP `Authorization` header during the WS upgrade. There is no
nonce, no challenge-response, no per-connection key derivation. An
attacker who captures one handshake (no TLS — comment at `:9`) can
replay it indefinitely against the same server. The constant-time
compare at `:424-427` is good, but it's the wrong layer.
Severity: high if the backend is ever bound to a non-loopback
address (the warning at `service_main.cpp:2510` acknowledges this).
Fix: require TLS at this transport. WebSocket-over-TCP with bearer
auth is acceptable only on loopback.

**Finding 9.2 — Token rotation requires backend restart.**
`auth_secret_` is set at startup from `--auth` / `XINSP2_AUTH` and
held by the WS server for its lifetime. There's no rotation
endpoint. Severity: low (operational). Fix: add an admin command
behind the existing token to rotate it; broadcast a "reauth-
required" event to current clients.

**Finding 9.3 — Auth gate checks header presence, not authentication
of the post-handshake frame stream.** Post-handshake all frames are
trusted on the basis of one TCP connection. If a load balancer
terminates and reuses connections (it shouldn't with WS, but…),
state leaks across clients. Severity: very low. Same fix as 9.1
(TLS + per-message integrity).

## 10. `xi::Param<T>` thread safety

**Finding 10.1 — Atomic value, but no change-notification path
exists.** `xi_param.hpp:117-127`. `value_` is `std::atomic<T>`,
reads/writes are race-free. There is no subscription / observer /
on-change callback in the surface. So claims about "the
subscription path racing" don't apply — there is no subscription
path. The script reads `Param` via implicit conversion every frame;
the change is observable on the next frame's read. Eventually
consistent. Severity: no finding here, the design is intentionally
notification-free.

**Finding 10.2 — Range mutation race.** `range_` (`:203`) is a plain
`Range<T>` non-atomic. `set` (`:122`) reads `range_.min/max`. If a
future feature mutates `range_` (today nothing does), readers race.
Worth annotating now: range is set-once-at-construction; document
that or wrap in atomics if mutability is added. Severity: very low.
No fix required today.

**Finding 10.3 — `set_from_json` on non-existent param + concurrent
ParamRegistry mutation.** The call sequence at
`service_main.cpp:1309-1325` is: `find()` returns a raw
`ParamBase*`, then `set_from_json(*p)` is called *outside* any
ParamRegistry lock. If the script reloads between `find()` and
`set_from_json` and the new DLL's destructor calls
`ParamRegistry::remove(this)`, the pointer is dangling. The
registry's `remove` (`xi_param.hpp:67-70`) erases from the map but
the Param object itself is destroyed by the script DLL's static
dtors during `FreeLibrary` in the load path
(`service_main.cpp:960`). The set_param command path is on the
same thread as load (both go through `handle_command`), so they're
serialized in single-client land. **However** the breakpoint
worker thread (`g_bp_cv` notified at `:818`) and the trigger
worker (`g_ev_cv`) can call into script methods independently —
not into ParamRegistry directly, but the precedent that "registry
+ raw pointer" is held across a potential reload is dangerous.
Severity: low today; lurking. Fix: make ParamRegistry hand out
shared_ptr<ParamBase> or grab the registry mutex for the duration
of the set call.

---

## Summary table

| # | Area | Severity | Fix complexity |
|---|---|---|---|
| 1.1 | xi_record_out cross-CRT alloc | high | medium (route through host_api) |
| 1.2 | record_from_c flatten | medium | low |
| 3.1 | tid id_key collision | high | low (use 128-bit key) |
| 3.2 | AllRequired evict-then-replace | medium | medium |
| 6.1 | replay tid vs live | medium | low |
| 6.2 | replay source-order non-determinism | medium | low |
| 7.1 | TerminateThread holds ImagePool shard | high | high (process isolation) |
| 8.1 | cert size+mtime not a digest | medium | low (SHA-256) |
| 9.1 | bearer auth replay (no TLS) | high if non-loopback | medium (TLS) |
| 2.1 | Future leak after watchdog | medium | medium |
| 4.2 | no state schema migration hook | medium | medium |
| 5.1 | ctor-throw leaks host images | medium | low |
| 8.3 | cert failure UX silent | medium | low |
| others | low / by-design / lurking | — | — |

Top 5 to fix this quarter: 3.1, 1.1, 8.1, 7.1 (long path), 9.1.
