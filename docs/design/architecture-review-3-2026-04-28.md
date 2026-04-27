# xInsp2 Architecture Review 3 — 2026-04-28

Third pass. Companion to `architecture-review-2026-04-28.md` (breadth) and
`architecture-review-deep-2026-04-28.md` (concurrency / lifetime). None of
those findings are repeated here. Scope: performance under load, file
durability, test coverage holes, API surface consistency, doc drift,
steady-state memory, error reporting, build/dependency story, AI-SDK gaps,
plugin packaging.

---

## 1. Performance under load — concrete cliffs

### 1.1 `emit_vars_and_previews` allocates 32 MB raw + JPEG buf per image, every frame
`backend/src/service_main.cpp:509` does `std::vector<uint8_t> raw(32 * 1024 * 1024);`
inside the per-gid loop. At 30 fps × 4 image-vars/cycle that is **3.84 GB/s**
of zero-init churn, which the OS commit-on-touch will mostly skip — but every
allocation still hits `operator new`, every release hits the heap. With 4
images per cycle this is 4 × 32 MB malloc/free pairs **per frame**, plus a
second `std::vector<uint8_t> jpeg` (`:513`) that grows per encode.

Severity: **high** for the headline 30 fps × 4-image case; the system spends
more time in heap than in `tjCompress2`. libjpeg-turbo at 2.71 ms/MP would
encode a 1080p RGB image in ~5 ms, but the surrounding code adds 2–4 ms of
allocator + `dump_image` copy.

Repro: 4 image VARs at 1920×1080 RGB, 30 fps `cmd:start`. Wall-clock
inspect time should be ~22 ms (4 encodes × ~5.5 ms turbo). Measured will be
materially higher with bigger spread under contention.

Proposed fix:
- Per-WS-session preview scratch: a single `std::vector<uint8_t> raw_buf;`
  and `std::vector<uint8_t> jpeg_buf;` reused across calls (reset to size,
  never deallocated). Sizes the buffers once and amortizes.
- Tighter: query dimensions via `s.dump_image(gid, nullptr, 0, &w, &h, &c)`
  and resize to exactly `w*h*c`. The current code blindly allocates 32 MB
  every time even for a 320×240 thumbnail.
- Even tighter: thread-local `tjhandle` is already in `encode_jpeg_turbo`
  (`xi_jpeg.hpp:143`) — extend the same pattern to the host-side raw buffer.

### 1.2 String-scan over every snapshot to discover gids
`service_main.cpp:489-505` does `find("\"name\":\"")` and `find("\"gid\":")`
inside the snapshot byte-stream **on every frame**. At 100 fps × 1 image
this is OK; at 30 fps × 20 vars (8 of them images, the rest scalars) the
scan walks ~50 KB of JSON 20 times. Worse: it is also fragile to JSON
output that interleaves `name` / `gid` order, which the script DLL's
`snapshot()` does not currently guarantee in any documented contract.

Severity: **medium**. Performance is real but bounded; the structural
concern is that a future snapshot format that reorders fields silently
loses preview-binding for all images.

Proposed fix: change `xi_script_snapshot` ABI to either (a) emit a parallel
`name→gid` index alongside the items array, or (b) parse with cJSON
(already linked) once per frame. cJSON parse of 50 KB JSON is ~100 µs;
the current 20× substring scan is 20–100 µs and brittle.

### 1.3 `cmd:run` spawns a fresh `std::thread` every call
`service_main.cpp:1159` `std::thread([&srv, run_id]() { ... }).detach();`.
A per-frame thread create is ~50–100 µs on Windows (CreateThread + TLS
init + `_set_se_translator` install). Inside continuous mode the
`g_worker_thread` is reused, so that path is fine. But every external
`cmd:run` (the AI agent's `c.run()` in a tight loop) pays the spawn cost.
At 100 fps × `cmd:run`, that's 5–10 ms/sec wasted on thread create alone.

Severity: **medium**. The detached-thread shape exists so the watchdog can
TerminateThread without killing the WS poll loop — fair. But the thread
is recreated on every call.

Proposed fix: a single long-lived "inspect dispatcher" thread that pulls
from a queue. `cmd:run` posts (`run_id`, ack-promise) and returns. The
dispatcher thread re-arms the watchdog per-iteration. Reduces 100 thread
creates/sec to one once.

### 1.4 ImagePool shard contention under multi-source TriggerBus
`xi_image_pool.hpp:69` takes `unique_lock` on `shard.mu` for every
`create()`. For 16 shards and ~2 cameras emitting every frame, the
contention is negligible. But: `xi_trigger_bus.hpp` releases image refs
in the dispatch path (`service_main.cpp:1223`); a 4-camera AllRequired
configuration releases 4 handles per frame, plus the inspect path
acquires/releases its own working set. Plugins in the loop using
`image_create` for transient outputs can produce 50+ create/release per
inspection cycle.

Severity: **low** today. SHARD_COUNT=16 with random handle distribution
gives ~6% same-shard collision per pair — fine. But for a 60 fps inspect
with 50 ops/cycle = 3000 lock acquisitions/sec the shard mutex is a
real, if minor, hot-spot.

Proposed fix (small): bump shard count to 64 (one cache-line per shard).
Proposed fix (large): per-thread free-list of recycled `PoolEntry`
buffers keyed on `(w,h,ch)`, fed back on `release()` when refcount hits
0. Avoids `pixels.resize(...)` re-zeroing 6 MB on every allocation.

---

## 2. File durability under power loss / hard reset

`xi_atomic_io.hpp:39-82` is well-intentioned but the durability claim is
weaker than the doc comment implies.

### 2.1 No `fsync` / `FlushFileBuffers` before rename
`atomic_write` does `f.flush()` (`:61`) — that flushes the C++ stream
buffer to the OS write cache only. The subsequent `MoveFileExW` carries
`MOVEFILE_WRITE_THROUGH` (`:67`), which the docs say flushes for the
*rename metadata operation* — not the file data. On a power loss between
write and rename, the directory entry can flip to the new file while the
file's data blocks still sit in the disk's write cache, leaving a
zero-length or torn `instance.json`.

Severity: **medium**. Win32 `MOVEFILE_WRITE_THROUGH` is documented as
flushing the rename, not the underlying file data on NTFS lazy-write.
The right primitive is `FlushFileBuffers` on the tmp handle before close.

Repro: write 5 KB instance.json on consumer SSD with cache enabled, kill
power between `f.flush()` and `MoveFileExW` return — recovery shows
file present but truncated.

Proposed fix:
```cpp
HANDLE h = CreateFileW(tmp.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
FlushFileBuffers(h);          // <-- the missing line
CloseHandle(h);
MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
```

Drop the `<fstream>` path on Windows; the fstream destructor's flush is
not equivalent.

### 2.2 Multi-instance `cmd:save_project` is not atomic across files
`save_project_locked` calls `save_instance_json` per instance
(`xi_plugin_manager.hpp:1458-1466`); each one is independently atomic,
but the *set* is not. A power loss after instance 3/5 leaves a project
where the on-disk layout disagrees with the still-in-memory state.

Severity: **low-medium**. Recovery is "old values for instances 4 and 5"
— defensible — but there is no "manifest version" in `project.json`
that pins which instances are at which state.

Proposed fix: write a `project.json` last that lists `(instance_name,
config_hash)` for each instance, with each `instance.json` containing
the same hash. On load, mismatch ⇒ warn "instance N reverted" rather
than silently using stale data.

### 2.3 `cert.json` partial write recovery
`xi_cert.hpp:75` uses `atomic_write`, so the cert itself is fine. But on
half-written **DLL** + crash, `is_valid()` checks
`(dll_size, dll_mtime)` (`:113-114`) — both can match a torn DLL whose
bytes aren't actually on disk. The cert claims "valid" against a DLL
the OS can't load. Severity: low (the LoadLibrary call fails in any
case), but the user-visible message is "factory not found" rather than
"DLL torn, re-cert needed".

Proposed fix: add SHA-256 of DLL bytes to cert (matches deep-review
finding 8.1).

### 2.4 Recovery story for half-written `instance.json` on backend reboot
There is none documented. `g_plugin_mgr.open_project` reads instance.json
(`xi_plugin_manager.hpp` open path); a malformed JSON throws to
`last_open_warnings_`. The instance is skipped. The factory can't be
re-instantiated until the user manually edits or removes the file.

Severity: **medium** for production-headless setups using
`xinsp-runner.exe` — a single bad reboot can require manual filesystem
intervention to restart inspection.

Proposed fix:
- On `open_project` startup, if any `instance.json` parse fails, look
  for `instance.json.tmp` sibling — if present and parseable, log "torn
  write recovered" and replace.
- If neither parses, fall back to `instance.json.bak` (a one-deep
  generational backup written before each save).

---

## 3. Test coverage holes — ranked

`docs/testing.md` is honest about what runs. The gaps below are what I'd
write today if I owned the test suite.

### 3.1 No test exercises `cmd:save_project` / `cmd:load_project` round-trip with project plugins
`ws_project.test.mjs` covers save/load but only against script-defined
instances. The plugin-manager-instance path
(`service_main.cpp:1393-1395`, `set_def`-via-`InstanceRegistry`) is
unreached. A failing `plugin.set_def(...)` returns `false` and produces
"set_def returned false" — but no test actually checks what happens
when a project on disk references a `def` shape the plugin's current
version no longer accepts. **Severity: medium** — this is exactly the
case that bites when a user upgrades a plugin DLL.

### 3.2 No test for `g_history` snapshot accumulation under continuous mode
`runHistory` covers depth resize and `since_run_id` filter. It does
**not** assert on memory bound: at the default 50-deep ring with 50 KB
JSON snapshots, that's 2.5 MB resident. Tests run for seconds; nothing
proves the ring actually evicts under hours of `cmd:start`. A bug where
`g_history.pop_front()` is gated on the wrong condition would not show
up in CI. **Severity: low** (visual inspection of the code shows the
`while` loop is correct), but the hole is real.

### 3.3 No test of `set_param` returning `false` from the script registry
`ws_commands.test.mjs` covers happy-path `set_param`. The error path
(`service_main.cpp:1331`, `send_rsp_err(srv, id, "set_param: bad value")`)
is unreached. Edge cases: out-of-range value, type mismatch
(`Param<int>` set with `"hello"`), name clash between script-param and
backend-param (which wins?). The clash question is structural — code
at `:1291-1306` says "Try the loaded script first, then fall back to
backend registry". A user with a script `Param<float> sigma{"sigma",...}`
and a backend-side identically-named param will silently set the
script's; the backend's never updates. **Severity: medium**, no test.

### 3.4 `cmd:exchange_instance` plugin-crash path has no test
`service_main.cpp:1432` catches `seh_exception` from a plugin
`exchange()` that null-derefs. There is `ws_crash.test.mjs` but it tests
inspect crashes, not exchange crashes. An AI agent driving a plugin's
UI buttons over `exchange_instance` is the most likely scenario for
this code path. **Severity: medium**.

### 3.5 `cmd:recompile_project_plugin` failure-then-success path
`runProjectPluginJourney` covers the typo-then-fix flow. But the test
asserts only on diagnostics shape, not on whether instances actually
reattach with previous defs intact. The reattach path
(`recompile_project_plugin` returning `reattached:[...]`) is the
linchpin claim of the doc — no test asserts that "set_def with the
saved value succeeded" after recompile. A regression would silently
ship instances with default state. **Severity: high** for the live-tune
workflow.

### 3.6 No fuzzer / invalid-JSON suite for `plugin.json`
`ws_adversarial.test.mjs` fuzzes WS frames; nothing fuzzes the file
inputs. Both deep-review finding 3.3 and the manifest pass-through path
read user-supplied files. A 1-line test that drops 100 random byte
mutations into a valid `plugin.json` and ensures the backend logs and
keeps running would catch a class of crash. **Severity: low** today.

### 3.7 `g_continuous` race on rapid start/stop
`service_main.cpp:1180-1185` does
`g_continuous = false; g_worker_thread.join(); g_continuous = true;`.
A test that hammers `start`/`stop` 100x in a loop verifies no orphan
worker threads and no leaked `g_ev_queue` images. None exists today.
**Severity: medium**.

### 3.8 No test for `xi::atomic_write` torn-write recovery
Per §2.4 above. A test that writes half a buffer + hard-fault would
require WinAPI mock — but a softer test (delete `.tmp`, ensure
`instance.json` still loadable) is easy. **Severity: low**.

### 3.9 No backend memory-residency assertion
None of the e2e suites do `GetProcessMemoryInfo` before/after a 1-min
continuous run. Steady-state RSS is the most common production "bug
report" shape ("backend now uses 2 GB after 8 hours") and the test
suite has zero coverage. **Severity: high** for production confidence.

---

## 4. API surface consistency

### 4.1 `set_param` vs `set_instance_def` divergent error shapes
- `set_param` (`service_main.cpp:1284-1332`): returns `rsp:ok=true` on
  success, `rsp:ok=false, error:"set_param: bad value"` on out-of-range.
  Clamping is silent (the `Param::set` clamps, but `set_from_json`
  reports the clamp didn't fail).
- `set_instance_def` (`:1379-1406`): returns `rsp:ok=true` on success,
  `rsp:ok=false, error:"set_def returned false"` on plugin rejection.
  No structured reason from the plugin.

From a script's `xi::use("cam0").exchange(...)` perspective both look
like binary success/fail. The plugin author wrote a `bool set_def(...)`
that returned `false` because "exposure_us out of range [10, 50000]" —
that string never reaches the user.

Severity: **medium**. Concrete fix: change the plugin-side `set_def`
contract to return `std::string error_or_empty` (deprecation path:
keep `bool` overload for one release, prefer string overload). Surface
the message in `rsp.error`.

### 4.2 `cmd:run` returns `ms:0` always
`service_main.cpp:1149`: `R"({"run_id":%lld,"ms":0})"`. The actual
inspect duration is unknown to the client until it ties together
`run_started` / `run_finished` events — and `run_finished` carries
`ms` per `docs/protocol.md:118` but the doc does not say `cmd:run`'s
`data.ms` is always 0. Drift.

Severity: **low** but documentation drift. Fix: either delete `ms`
from the `cmd:run` reply (it's always 0 anyway) or back-fill it from
`run_finished` before responding (would require holding the rsp until
inspect completes — undesirable).

### 4.3 List commands name-shape mostly consistent, with one exception
`list_plugins`, `list_instances`, `list_params` — fine.
`get_project`, `get_plugin_ui` — different verb. `get_project` returns
the plugin manager's full state including instances and params — so
its name should be `list_project` or `project_state`. `get_plugin_ui`
returns a path; that's a get. Consistency lapse: small, but the
`list_*` family doesn't include `list_history` (that's just `history`).

Severity: **low**. Fix: alias `cmd:project_state` to `cmd:get_project`,
deprecate the old name in 1 release.

### 4.4 `recording_start` vs `recording_stop` vs `recording_replay`
Inconsistent with the `<noun>_<verb>` shape elsewhere:
- `save_project` (verb-noun)
- `load_project` (verb-noun)
- `recording_start` (noun-verb)
- `recording_replay` (noun-verb)

If a user types `start_recording` / `replay_recording` they get
`unknown command`. Severity: **very low**, cosmetic. Fix: add aliases.

### 4.5 `cmd:script_isolated_run` vs `cmd:run`
The isolated path is a separate command rather than a flag. From an AI
agent's perspective two commands both called "run" with different
behaviors is confusing. Severity: **low** — but the moment isolation
goes default-on (per the orchestrator note), this command should fold
into `cmd:run` with `args:{isolation:"process"}` and the legacy command
become a synonym for one cycle.

---

## 5. Documentation drift — 5 cmds + 3 plugins audited

### 5.1 `cmd:run` — `frame_path` documented (`protocol.md:172`), ignored by handler
Already covered in review #1. Still drift.

### 5.2 `cmd:list_plugins` — `cert` field undocumented at protocol level
Doc says reply has `name`, `description`, `folder`, `has_ui`, `loaded`,
`origin`, `cert`, `manifest`. The `cert` shape inside (`present`,
`valid`, ...) is shown only as `...` — actual fields emitted by
`xi_plugin_manager.hpp` are `present`, `valid`, `tests_passed`,
`certified_at`, `duration_ms`. Severity: low. Fix: spell out cert
schema in `docs/protocol.md`.

### 5.3 `cmd:save_project` reply shape vs implementation
Doc (`protocol.md:208`): "→ ok: true". Implementation
(`service_main.cpp:1481-1485`) calls `send_rsp_ok(srv, id)` with no
data — matches. ✓

### 5.4 `cmd:set_param` doc vs reply
Doc (`protocol.md:197`): "ok: true". Implementation: returns either
`rsp:ok` or `rsp:ok=false, error:"set_param: bad value"`. Doc omits
the failure shape. Severity: low.

### 5.5 `cmd:open_project_warnings` documented as "planned, not yet wired"
`protocol.md:242-249`. Confirmed unwired in handler — search shows no
`name == "open_project_warnings"` in `service_main.cpp`. Doc is
honest about it being unimplemented. ✓ — but for the AI-agent contract,
"this command exists but doesn't work" is worse than "this command
doesn't exist". Either ship it or remove the doc section.

### 5.6 Plugin manifests vs `plugin-abi.md`
`plugin-abi.md:253-268` documents an optional top-level `manifest`
block with `params`, `inputs`, `outputs`, `exchange`. Audited shipped
plugins:
- `plugins/blob_analysis/plugin.json` — **no `manifest` block**.
- `plugins/json_source/plugin.json` — **no `manifest` block**.
- `plugins/mock_camera/plugin.json` — **no `manifest` block**.

All three are exactly the 6 documented required fields, no extras. The
review #1 callout that "Manifest pass-through is a quietly excellent
piece of design" is true at the framework level — but **no shipped
plugin uses it**. So an AI agent calling `cmd:list_plugins` on a fresh
backend gets zero `manifest` blocks back, despite the SDK and docs
claiming that's the introspection mechanism.

Severity: **high** for the "AI-agent introspects plugins" narrative.
Fix: backfill manifests into all 7 shipped plugins. Each is ~30 lines
of JSON. Without this, every AI-agent experiment has to grep source
to discover params — exactly the friction the manifest mechanism was
supposed to remove.

---

## 6. Memory consumption / steady state — accumulation points

A backend running at 10 fps × 1 hour = 36000 inspections. Where does
memory land?

### 6.1 `g_history` ring — bounded, fine
`service_main.cpp:471-475` evicts at `g_hist_max` (default 50). At
~50 KB JSON × 50 = 2.5 MB cap. ✓

### 6.2 `last_open_warnings_` — unbounded across project lifetime
`xi_plugin_manager.hpp` accumulates warnings without a cap. A project
that re-opens 1000 times accumulates 1000 entries. Severity: low.
Fix: cap to last 100 entries.

### 6.3 Plugin-side cJSON inside long-lived `Record`s
A plugin that stashes a `xi::Record` member and accumulates entries
via `set(...)` without a `Record clear` will grow without bound. Not
the framework's bug per se, but `xi::Record` has no `clear()` method
documented (`xi_record.hpp` has no `clear()` API in the visible part).
Plugin authors who think "I'll just keep appending stats per-frame" leak
forever. Severity: **medium** for plugin-author footgun. Fix: add
`Record::clear()` and document the lifetime expectation.

### 6.4 `TriggerRecorder::events_` (covered in deep-review 6.4)
Deep-review noted unbounded growth. Confirmed. Skipping detail.

### 6.5 `LeaderFollowers::follower_latest_` (covered in deep-review 3.3)
Same.

### 6.6 Image handles leaked by script `xi::Image` outliving a frame
The "AI-driven exploration" pattern of `xi::Image roi = full.crop(...)`
inside a script — if the script keeps `roi` as a class member that
survives across `inspect()` calls, the underlying handle's refcount
holds the pool entry alive. With 36000 frames × 1 such handle/frame
unreleased = pool grows to 36000 entries, ~150 GB at 1080p RGB.

Severity: **high** if any script does this — and AI-generated scripts
are the most likely to. There is no diagnostic surface today; the
`ImagePool` doesn't expose a "current entry count" for monitoring.

Proposed fix:
- Add `ImagePool::stats()` returning entries-per-shard + total bytes.
- Surface as `cmd:pool_stats` event on every Nth frame in continuous
  mode, with a configurable warning threshold.
- Document the lifetime contract in `xi_image.hpp` ("Image handles
  released when the variable goes out of scope; do NOT store across
  inspect() calls — use `xi::state` instead").

### 6.7 cJSON nodes inside `g_persistent_state_json`
`service_main.cpp` keeps the script's `xi::state()` JSON across
reloads. If the script's state grows monotonically (e.g., a bug where
`state["history"].append(...)` instead of `state["last"] = ...`), the
JSON grows forever. After 1 hour at 10 fps with a misbehaving script,
state could be hundreds of MB. The buffer grow-and-retry at `:954-957`
keeps working — *that's the bug*. There's no upper cap.

Severity: **medium**. Fix: cap state size at, say, 64 MB, log error
and refuse `set_state` beyond that.

---

## 7. Error reporting consistency from script's perspective

Three different channels:

1. **`xi::use("name")` failures** — if `cam0` doesn't exist, the
   accessor returns a default-constructed instance proxy. Calls into
   it silently no-op or throw `std::runtime_error`. Different per
   plugin type.

2. **Plugin crashes mid-process** — caught by `seh_translator` in the
   inspect thread; surfaces as a `run_error` event (`protocol.md:361`).
   But `cmd:run`'s `rsp` already returned `ok:true`. Script code
   continues after the call site; the inspection loses output silently
   from the script's local view.

3. **`set_def` returns `false`** — surfaces as `rsp:ok=false, error:"
   set_def returned false"`. From a script that does
   `xi::use("cam0").set_def(j)` (assuming such an API), the bool comes
   back as `false`.

These three error channels are independent. The AI agent has no unified
"give me the last error from any source" command.

Severity: **high** for the AI-agent workflow. Fix: introduce a
`cmd:get_last_error` returning `{run_id, source: "plugin"|"script"|"
host", message, code, stack_trace?}`. Populated by every error path.
Keeps a ring of last 32 errors.

Severity-aside: from a script's `xi::use<Camera>("cam0").process(input)`
when `cam0` doesn't exist — read `xi_use.hpp`. Actual behavior: the
template returns a proxy that calls into the script-side instance
registry; `find()` returns null; the proxy throws `std::runtime_error
("instance not found: cam0")`. That throws inside `inspect()`; SEH
catches it and emits `run_error`. So "use a missing instance" → silent
in `vars`, surfaces in `event` channel. Documented nowhere.

---

## 8. Build / dependency story

### 8.1 No global guard prevents a plugin source from including `<opencv2/...>` unconditionally
`plugins/CMakeLists.txt` has zero `XINSP2_HAS_OPENCV` references.
Currently no shipped plugin uses OpenCV — so by accident, this is
fine. But a plugin author who writes
`#include <opencv2/imgproc.hpp>` at the top of their `.cpp` will get a
hard compile error if OpenCV isn't installed, with a generic "cannot
open include file" message that has nothing to do with xInsp2.

Severity: **medium** for the plugin-author-onboarding story. Fix: add
the optional include resolution to `plugins/CMakeLists.txt` so a
plugin built against an OpenCV-less host fails fast with a clear
message ("plugin foo uses OpenCV but XINSP2_HAS_OPENCV=OFF in this
backend build").

### 8.2 Fresh-checkout to running backend on a clean Windows box (no IPP, no turbojpeg, no OpenCV)
Path: `cmake -DXINSP2_HAS_OPENCV=OFF`, build, run. Backend builds in
~3 min on warm cache, ~7 min cold. `encode_jpeg` falls back to stb
(120 MP/s — 17 ms/encode for 1080p, vs 2.7 ms with turbo). Op
performance falls back to portable C++ (gaussian becomes 26 ms vs
1.16 ms with OpenCV — 23× slower). Continuous mode at 30 fps becomes
unworkable.

Severity: **medium** documentation gap. The `docs/testing.md`
performance baseline shows the numbers but doesn't say "without
optional accelerators, 30 fps continuous mode is not viable for 1080p
RGB". Fix: add a "what you get without each optional dep" table to
the build guide.

### 8.3 `OpenCV_DIR` autodetect logic is Windows-only (`backend/CMakeLists.txt:64-74`)
Linux fresh-checkout with OpenCV in `/usr/lib/cmake/opencv4` works
because `find_package` finds it; Windows without `OpenCV_DIR` set and
without OpenCV in the tried locations fails with a warning, not an
error. Build still proceeds, ops degraded silently. Severity: low.

---

## 9. AI-driven workflow specific — `tools/xinsp2_py` SDK gaps

`xinsp2/client.py` is 251 lines, exposes: `connect`, `call`, `ping`,
`version`, `compile_and_load`, `load_project`, `open_project`,
`recompile_project_plugin`, `set_param`, `run`, `on_log`. That's the
whole high-level API.

For an AI agent doing closed-loop inspection authoring, the missing
methods are:

### 9.1 `c.list_plugins()` / `c.list_instances()` / `c.list_params()`
Today: `c.call("list_plugins")` works but the result is a raw cJSON
shape. The agent has to parse manifests itself. Add:
```python
def list_plugins(self) -> list[PluginInfo]:
    """Returns dataclasses with parsed manifest, cert status, has_ui."""
def list_params(self) -> list[ParamInfo]:
    """Includes range, default, current value, type."""
```

### 9.2 `c.set_def(instance_name, def_dict)` and `c.get_def(instance_name)`
Today: agent must `c.call("set_instance_def", {"name":..., "def":...})`.
Add typed wrappers with validation against the manifest (which is
what manifests are for — see §5.6).

### 9.3 Structured error reporting
Per §7. Add `c.last_error()` that pulls from the `cmd:get_last_error`
backend ring. Today the agent can only listen for `event` messages
between calls and try to associate them with the previous call.

### 9.4 Run-id provenance for snapshots
`c.run()` returns a `RunResult` with `run_id` (`client.py:189-195`).
But there's no way to ask "what set_param values were active for this
run?" or "what's the project hash?" — both critical for an AI agent
trying to learn from snapshots over a long session. Add:
```python
@dataclass
class RunSnapshot:
    run_id: int
    ts_ms: int
    params: dict[str, Any]
    instances: dict[str, dict]  # name → def
    project_hash: str           # SHA-256 of project.json content
    vars: list[dict]
    previews: dict[int, PreviewFrame]
```
Backend side: `g_history` entries should carry params + instance defs
at the time of the snapshot, not just vars. The agent can then
reconstruct the experimental conditions of any historical run.

### 9.5 Mid-script pause/inspect — `xi::breakpoint` is wired but agent SDK has no method
`runBreakpoint.mjs` exercises `cmd:resume`. The Python SDK has neither
`pause()` nor `step()` nor `resume()`. An agent debugging a crash
inside a long inspect can't poke around and continue. Add:
```python
def wait_for_breakpoint(self, timeout: float) -> dict | None:
    """Block until next 'breakpoint' event. Returns label + last vars."""
def resume(self) -> None:
    """Send cmd:resume; raises if nothing paused."""
```

### 9.6 Schema query — "what's the type of VAR(x)?"
Today's `vars` items carry `kind` (image|number|bool|string|json) but
not stricter type info — a `number` could be int or float, a `json`
could be anything. For an agent generating downstream code that reads
a VAR, it has to infer types from runtime samples. Add a
`cmd:var_schema` (planned new command) that returns the script's
declared VAR shapes — extracted from the .cpp source by the compiler
front-end during `compile_and_load` (`xi_script_compiler.hpp` already
has the AST). Method: `c.var_schema() -> dict[str, VarType]`.

### 9.7 `c.preview_instance(name)` — exists in backend, missing in SDK
`cmd:preview_instance` (`service_main.cpp:1540-1574`) lets a client
JPEG-grab from any ImageSource without running an inspect. AI agents
exploring a new project want this for "what does this camera see right
now?" without affecting state. Add `c.preview_instance(name) ->
PreviewFrame | None`.

### 9.8 `c.exchange(instance, cmd)` — missing
The exchange channel is a plugin's main RPC surface
(`plugin-abi.md:160-177`). Python SDK has nothing for it. An agent that
wants to drive a plugin's UI (e.g., `record_save`'s "save now" button)
has to fall back to raw `c.call`. Add:
```python
def exchange(self, instance: str, cmd: dict | str) -> dict:
    """Plugin-defined RPC. Returns parsed JSON or raises ProtocolError."""
```

---

## 10. Plugin distribution / packaging

`cmd:export_project_plugin` (`service_main.cpp:1815-1851`,
`xi_plugin_manager.hpp:596`) compiles release + runs cert + drops
`{plugin.json, <name>.dll, cert.json}` into `dest_dir`.

### 10.1 Drop-in to another machine — works iff vcruntime + ABI version match
The exported folder contains:
- `plugin.json` (the manifest).
- `<name>.dll` compiled with the source machine's `cl.exe` toolchain.
- `cert.json` with `dll_size`, `dll_mtime`, `baseline_version`.

On the target machine:
- Different `vcruntime140.dll`? Crashes on first `process()` call (per
  deep-review 1.1).
- Different `xi_abi.h` version (target backend older)? `xi_record_out`
  layout could differ. There is **no version check** at load time. The
  cert's `baseline_version` is the only check, and that is the host's
  baseline test version — not the ABI version.

Severity: **high**. Concretely: a plugin built against `xi_abi.h` v2
loaded by a backend at v1 reads garbage off `xi_record.image_count`.

Proposed fix:
- Add `XI_ABI_VERSION` integer constant in `xi_abi.h`.
- Plugin export embeds it in `plugin.json` as `"abi": <int>`.
- Loader reads `plugin.json.abi`; refuses load if `> host's abi`,
  warns if `< host's abi - K` (where K is the back-compat window).
- Cert tests need `min_abi`, `max_abi` fields too.

### 10.2 No "what version of xInsp2" stamped in exported folder
Beyond ABI version, knowing "this DLL was built against xInsp2 commit
abc123" is critical for triaging field issues. Today the cert has
`certified_at` (timestamp) and `baseline_version`. No commit hash, no
xInsp2 release tag.

Severity: **medium**. Fix: extend cert with `xinsp2_commit`,
`xinsp2_version` (already known via `XINSP2_VERSION` /
`XINSP2_COMMIT` macros at compile time, see `service_main.cpp:441`).

### 10.3 No bundling of vcruntime / OpenCV DLLs in export
`xinsp_deploy_dlls` (`backend/CMakeLists.txt:325-347`) copies optional
DLLs next to the **backend** exe. Plugin exports do not get the same
treatment — but the plugin compiled against OpenCV does need
`opencv_world*.dll` next to it on the target. The export folder is
not self-contained for accelerator-using plugins.

Severity: **medium**. Fix: `export_project_plugin` should optionally
bundle the optional DLLs the plugin links to (PE-import-table walk to
discover, or just copy known accelerator DLLs from the backend's
runtime folder). Doc the limitation today.

### 10.4 No back-compat strategy beyond "append-only struct evolution"
The decision-log claim is plugins compiled in 2024 still load in 2030.
That works **iff** the host always reads only fields the plugin
populated. The current `xi_record_out` API has fixed layout; new fields
mean a new layout. There is no `version` byte at the start of
`xi_record` to discriminate. So today's "stable ABI" is really "frozen
ABI" — to evolve it, a v2 entry-point name (`xi_plugin_create_v2`) is
required.

Severity: **low** today (works for current scope) but creates a slow
trap as the framework evolves. Fix: add a version byte to `xi_record`
+ `xi_record_out`; loaders look at it to gate field access.

---

## Summary table

| # | Area | Severity | Cost |
|---|---|---|---|
| 1.1 | 32 MB malloc per-image per-frame in `emit_vars_and_previews` | high | low (reuse buf) |
| 1.2 | Snapshot string-scan for gids | medium | medium (re-shape ABI or cJSON parse) |
| 1.3 | Per-`cmd:run` thread spawn | medium | medium (single dispatcher) |
| 2.1 | `atomic_write` no `FlushFileBuffers` | medium | low |
| 2.2 | Multi-instance `save_project` not atomic across files | medium | medium |
| 2.4 | No half-write recovery for `instance.json` | medium | low |
| 3.5 | No test asserts plugin-instance reattach state after `recompile_project_plugin` | high | low |
| 3.9 | No memory-residency test | high | low |
| 4.1 | `set_def` failure has no structured reason | medium | medium |
| 5.6 | All 7 shipped plugins lack `manifest` block — AI introspection broken in practice | high | low (backfill JSON) |
| 6.6 | Image handles leaked by long-lived `xi::Image` in script | high | medium (add stats + doc) |
| 6.7 | No cap on `g_persistent_state_json` size | medium | low |
| 7 | Three independent error channels, no unified `last_error` | high | medium |
| 8.1 | `plugins/CMakeLists.txt` no `XINSP2_HAS_*` gating | medium | low |
| 9.x | Python SDK missing list_plugins/exchange/preview/breakpoint/var_schema | high (AI workflow) | medium |
| 10.1 | Plugin export has no ABI version check | high | low |
| 10.3 | Plugin export not self-contained for OpenCV plugins | medium | medium |

**Top 5 to fix this cycle**: 5.6 (plugin manifests — unlocks AI introspection
that is *already wired*), 9.x (SDK methods — same theme, additive code),
1.1 (preview buffer reuse — measurable fps win), 10.1 (ABI version stamp —
prevents silent breakage as ABI evolves), 7 (`last_error` — error
diagnosability is the foundation of an autonomous workflow).
