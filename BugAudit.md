# xInsp2 Bug Audit Report

Four parallel audit agents reviewed the entire core codebase against FRAMEWORK.md.
28 findings total. Categorized by severity.

---

## CRITICAL — Fix before any deployment

### A4-1: Command injection in script compiler
- **File:** `xi_script_compiler.hpp:115-146`
- **Issue:** All `CompileRequest` string fields (`source_path`, `extra_sources`, `include_dirs`, `output_dir`) are interpolated directly into a `cmd /C` shell string passed to `std::system()`. A path containing `" & calc.exe & "` escapes the quotes and executes arbitrary commands.
- **Risk:** Arbitrary code execution on the backend machine.
- **Fix:** Sanitize all paths (reject or escape shell metacharacters), or switch to `CreateProcess` API which doesn't involve a shell.

### A2-6: ImagePool handle truncation — collision at scale
- **File:** `xi_image_pool.hpp:65`
- **Issue:** `next_handle_` is `uint64_t` but cast to `uint32_t` for the handle. After 2^32 handles (~4.3 billion), values wrap and collide with live entries. At 1M handles/sec this is ~72 minutes.
- **Risk:** Silent data corruption — two different images share the same handle.
- **Fix:** Either use `uint64_t` handles throughout (change `xi_image_handle` typedef in `xi_abi.h`), or use a recycling freelist that checks liveness before reuse.

### A3-1: Race condition — worker thread holds dangling DLL function pointers
- **File:** `service_main.cpp:260-263`
- **Issue:** `run_one_inspection` copies `g_script` under `g_script_mu`, but the worker thread calls it in a loop while the WS thread can simultaneously handle `compile_and_load` which calls `unload_script` + `load_script`. The copied `LoadedScript` holds raw function pointers from the old DLL. If the DLL is unloaded between the copy at line 262 and the call at line 277, those pointers dangle.
- **Risk:** Use-after-free crash during hot-reload while continuous mode is running.
- **Fix:** Stop continuous mode before reloading. Or use a shared_ptr to a generation counter that invalidates old copies.

### A3-7: Shutdown during continuous mode — use-after-free
- **File:** `service_main.cpp:322`
- **Issue:** `cmd: shutdown` sets `g_should_exit` but never sets `g_continuous = false` or joins `g_worker_thread`. Main loop exits, `srv` is destroyed, and the worker thread calls `run_one_inspection` on a destroyed server.
- **Risk:** Crash on exit. In production, may corrupt logs or leave sockets half-open.
- **Fix:** Set `g_continuous = false`, join `g_worker_thread` before exiting main.

---

## HIGH — Fix before production

### A1-1: Future::get() double-call is undefined behavior
- **File:** `xi_async.hpp:43,46`
- **Issue:** `std::future::get()` is one-shot. Calling `get()` or `operator T()` twice on the same Future throws `std::future_error` at best, is UB at worst. No guard prevents this. Also, if a Future is destroyed without being awaited, the `std::async` destructor blocks silently.
- **Risk:** User accidentally writes `Image a = p1; Image b = p1;` → crash or hang.
- **Fix:** Add a `bool consumed_` flag. Second call returns cached value or throws descriptive error.

### A1-6: Image allows negative dimensions
- **File:** `xi_image.hpp:33-36`
- **Issue:** `Image(-1, -1, 3)` wraps via `static_cast<size_t>(-1 * -1 * 3)` = 3, creating a 3-byte buffer with w=-1, h=-1. `stride()` returns -3. Any pixel loop will overrun.
- **Risk:** Memory corruption from bad input.
- **Fix:** Assert or clamp dimensions to >= 0 in constructor.

### A1-8: xi::state() is not thread-safe
- **File:** `xi_state.hpp:37-39`
- **Issue:** `state()` returns a bare `Record&` to a function-local static. Record uses raw cJSON pointers internally (not thread-safe). Concurrent `xi::async` tasks calling `state().set(...)` and `state()["..."]` is a data race.
- **Risk:** Undefined behavior — corrupted JSON tree, crash.
- **Fix:** Add a mutex inside state(), or document that state() must only be called from the main inspect thread (not from async tasks).

### A3-2: cmd:run + worker thread both call inspect simultaneously
- **File:** `service_main.cpp:411`
- **Issue:** `cmd: run` calls `run_one_inspection` on the WS thread while the worker thread does the same concurrently. Both call `s.reset()` and `s.inspect()` without synchronization — the script DLL is not thread-safe.
- **Risk:** Data race in user script, corrupted ValueStore.
- **Fix:** Reject `cmd: run` while continuous mode is active, or acquire a mutex before inspect.

### A3-3: use_exchange_cb missing crash protection
- **File:** `service_main.cpp:110`
- **Issue:** `inst->exchange(cmd)` in `use_exchange_cb` is called without any try/catch. A crash in a plugin's exchange handler kills the backend. Compare with the SEH-guarded `use_process_cb`.
- **Risk:** Backend crash from plugin bug during script execution.
- **Fix:** Wrap in try/catch(seh_exception).

### A2-2: Input image handles leaked on process crash
- **File:** `xi_use.hpp:57-60`
- **Issue:** `from_image` creates handles (refcount=1), but if `process_fn` throws (SEH-caught crash), execution skips the release loop at line 71. Handles leak permanently.
- **Risk:** Memory exhaustion over time from repeated plugin crashes.
- **Fix:** Use RAII wrapper or scope guard to release handles on any exit path.

### A2-5: Plugin constructor exception crosses C ABI
- **File:** `xi_abi.hpp` (XI_PLUGIN_IMPL macro)
- **Issue:** `xi_plugin_create` calls `new ClassName(host, name)`. If the constructor throws a C++ exception, it propagates across the `extern "C"` boundary — undefined behavior.
- **Risk:** Process crash or corrupted stack on plugin construction failure.
- **Fix:** Wrap in try/catch, return nullptr on failure.

### A3-5: Output image handles leaked on JPEG encode failure
- **File:** `service_main.cpp:825-858`
- **Issue:** In `process_instance`, output image handles are only released if JPEG encoding succeeds. When `encode_jpeg` fails (line 842 `continue`), `oi.handle` is never released. Also when `to_image` returns empty, the handle leaks.
- **Risk:** Slow memory leak proportional to encode failure rate.
- **Fix:** Release output handle regardless of encode success.

---

## MEDIUM — Fix for robustness

### A1-7: VAR(x, "hello") hits wrong VarTraits
- **File:** `xi_var.hpp`
- **Issue:** `"hello"` decays to `const char*`, which hits the default `VarKind::Custom` trait, not `VarKind::String`. The value gets stashed as `std::any<const char*>` (dangling pointer risk if the literal came from a temporary).
- **Fix:** Add `VarTraits<const char*>` specialization that converts to std::string.

### A1-2: await_all broken with void Futures
- **File:** `xi_async.hpp:102-104`
- **Issue:** `std::make_tuple(fs.get()...)` fails to compile when any `Ts` is `void` because `void` cannot be a tuple element, despite the comment claiming "Void futures are accepted."
- **Fix:** Use `if constexpr` to filter void types, or remove the claim from the comment.

### A1-4: image_keys_json() doesn't escape keys
- **File:** `xi_record.hpp:349`
- **Issue:** `out += "\"" + k + "\""` injects raw key strings. If any image key contains `"` or `\`, the output is malformed JSON.
- **Fix:** Use the existing `escape()` helper.

### A2-1: HostImage public constructor is a refcount trap
- **File:** `xi_abi.hpp:54`
- **Issue:** The constructor calls `image_addref()`, but `image_create()` already returns refcount=1. Any code doing `HostImage(host, host->image_create(...))` will leak. `from_handle()` exists to avoid this, but the public constructor is still accessible.
- **Fix:** Make the (host, handle) constructor private. Only `from_handle()` and copy ctor should be public.

### A2-3: xi_record_out_free may free keys that Record references
- **File:** `xi_use.hpp:89-94`
- **Issue:** `to_image` copies pixels but `result.image(key, ...)` at line 90 may store a reference to the key string. `xi_record_out_free` at line 94 frees those key strings.
- **Fix:** Verify that `Record::image()` copies the key string (it does — `std::map<std::string, Image>` stores by value). Not a real bug, but verify.

### A2-4: CAbiInstanceAdapter never frees DLL handle
- **File:** `xi_plugin_manager.hpp:75-83`
- **Issue:** `CAbiInstanceAdapter` stores `HMODULE` but never calls `FreeLibrary`. DLL handle leaks on project switch.
- **Fix:** The DLL is owned by `PluginInfo`, not the adapter. Don't free in adapter — but add cleanup to `PluginManager` destructor.

### A3-4: process_instance missing catch(std::exception) path
- **File:** `service_main.cpp:787-797`
- **Issue:** SEH catch releases `src_h` and frees `output`, but there is no `catch(...)` or `catch(std::exception&)`. A C++ exception leaks both.
- **Fix:** Add `catch(const std::exception&)` and `catch(...)` cleanup paths.

### A4-2: WebSocket fragmentation not handled
- **File:** `xi_ws_server.hpp:419,446`
- **Issue:** `fin` is read but discarded. Multi-frame messages are silently lost. Not a crash but a correctness bug.
- **Fix:** Accumulate continuation frames until FIN=1, or reject fragmented messages with a close frame.

### A4-3: 64 MiB allocation from malformed frame header
- **File:** `xi_ws_server.hpp:434,438-439`
- **Issue:** A single malformed frame claiming 64 MiB triggers allocation before any payload arrives. DoS from localhost.
- **Fix:** Read payload incrementally, or reduce kMaxFrame for the localhost use case.

### A4-5: Image key JSON injection in snapshot thunk
- **File:** `xi_script_support.hpp:123`
- **Issue:** `out += "\"" + ik + "\":"` — a crafted image key containing `"` achieves JSON injection.
- **Fix:** Use the existing `esc()` helper for the key.

---

## LOW — Polish

### A1-3: Path parser key buffer 256 char truncation
- **File:** `xi_record.hpp:225-226`
- **Issue:** `char key[256]` silently truncates keys >= 256 chars. Wrong lookup, no error.
- **Fix:** Use `std::string` instead of fixed buffer.

### A1-5: Path [-1] parses as idx=0
- **File:** `xi_record.hpp:211-216`
- **Issue:** `[-1]` skips the digit loop and yields idx=0 silently.
- **Fix:** Check for non-digit characters after `[`.

### A1-9: Param as_json doesn't escape name
- **File:** `xi_param.hpp:141`
- **Issue:** A param name containing `"` produces malformed JSON.
- **Fix:** Escape the name.

### A3-6: Dead code — g_trigger_cv and g_trigger_pending
- **File:** `service_main.cpp:131-133`
- **Issue:** Declared, `notify_all()` called, but nothing ever waits on them. The worker thread uses `sleep_for`.
- **Fix:** Remove dead code.

### A4-4: Optional DLL symbols not null-checked
- **File:** `xi_script_loader.hpp:65-75`
- **Issue:** Only `inspect` is null-checked. All other `GetProcAddress` results stored without checks. Callers that invoke without checking crash.
- **Fix:** Callers already null-check before use. Document the contract.

### A4-6: Static globals in force-included header
- **File:** `xi_script_support.hpp:234-236`
- **Issue:** `g_use_*_fn_` statics are tied to the DLL lifetime. If backend retained references after FreeLibrary, it would dereference freed memory.
- **Fix:** Currently safe because backend calls `set_use_callbacks` after each load. Document the invariant.

### A4-7: data_json injected raw into protocol response
- **File:** `xi_protocol.hpp:173-175`
- **Issue:** `data_json` is appended verbatim. Malformed JSON in data_json produces malformed protocol messages.
- **Fix:** Validate with `cJSON_Parse` before sending, or document the trust boundary.

---

## Status tracking

| ID | Severity | Fixed? | Notes |
|----|----------|--------|-------|
| A4-1 | CRITICAL | ❌ | Command injection |
| A2-6 | CRITICAL | ❌ | Handle truncation |
| A3-1 | CRITICAL | ❌ | Worker dangling DLL pointers |
| A3-7 | CRITICAL | ❌ | Shutdown use-after-free |
| A1-1 | HIGH | ❌ | Future double-get |
| A1-6 | HIGH | ❌ | Negative image dimensions |
| A1-8 | HIGH | ❌ | state() thread safety |
| A3-2 | HIGH | ❌ | cmd:run vs worker race |
| A3-3 | HIGH | ❌ | use_exchange_cb no SEH |
| A2-2 | HIGH | ❌ | Handle leak on process crash |
| A2-5 | HIGH | ❌ | Plugin ctor exception across ABI |
| A3-5 | HIGH | ❌ | Output handle leak on encode fail |
| A1-7 | MEDIUM | ❌ | const char* VarTraits |
| A1-2 | MEDIUM | ❌ | await_all + void |
| A1-4 | MEDIUM | ❌ | image_keys_json escaping |
| A2-1 | MEDIUM | ❌ | HostImage ctor trap |
| A2-3 | MEDIUM | ❌ | record_out key lifetime |
| A2-4 | MEDIUM | ❌ | DLL handle leak |
| A3-4 | MEDIUM | ❌ | Missing catch paths |
| A4-2 | MEDIUM | ❌ | WS fragmentation |
| A4-3 | MEDIUM | ❌ | 64 MiB DoS alloc |
| A4-5 | MEDIUM | ❌ | JSON injection in snapshot |
| A1-3 | LOW | ❌ | Key buffer truncation |
| A1-5 | LOW | ❌ | [-1] parse |
| A1-9 | LOW | ❌ | Param name escaping |
| A3-6 | LOW | ❌ | Dead code |
| A4-4 | LOW | ❌ | Optional symbol docs |
| A4-6 | LOW | ❌ | Static globals fragile |
| A4-7 | LOW | ❌ | data_json trust boundary |
