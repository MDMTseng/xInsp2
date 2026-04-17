# xInsp2 Complete Test Plan

Target: 80%+ infrastructure coverage. Every layer has its own test file.
Tests are grouped by layer, not by feature, so each file can run independently.

---

## Layer Map

```
Layer 1: C++ Unit (backend/tests/)          — pure logic, no backend process
Layer 2: Node Integration (test/ws_*.mjs)   — one backend per test, WS commands
Layer 3: E2E (test/e2e/)                    — VS Code extension, screenshots
```

---

## Layer 1: C++ Unit Tests

### test_xi_core.cpp (existing — expand)

| # | Test | Status | Asserts |
|---|------|--------|---------|
| 1 | async basic: spawn + await | ✅ exists | value correct |
| 2 | async parallel wall time | ✅ exists | dt < threshold |
| 3 | async exception propagation | ✅ exists | catch at get() |
| 4 | ASYNC_WRAP | ✅ exists | value correct |
| 5 | **async double-get returns cached value** | ❌ add | `p.get() == p.get()`, no crash |
| 6 | **async void Future** | ❌ add | compiles, get() doesn't throw |
| 7 | **Future destroyed without await blocks** | ❌ add | verify no hang (timeout-guarded) |
| 8 | VAR tracks and binds | ✅ exists | name, kind, value |
| 9 | ValueStore thread-local | ✅ exists | isolation |
| 10 | **VAR with const char* literal** | ❌ add | kind == String, not Custom |
| 11 | **VAR with Image** | ❌ add | kind == Image, pixels preserved |
| 12 | **VAR with Record** | ❌ add | kind == Json, data_json correct |
| 13 | Param implicit read + clamp | ✅ exists | range enforcement |
| 14 | Param bool | ✅ exists | true/false |
| 15 | **Param name with special chars in as_json** | ❌ add | doesn't produce malformed JSON |
| 16 | Instance create + registry | ✅ exists | lookup, reuse |
| 17 | **Image negative dimensions clamped** | ❌ add | Image(-1,-1,3) → empty |
| 18 | **Image zero dimensions** | ❌ add | Image(0,0,0) → empty() true |

### test_record.cpp (NEW)

| # | Test | Asserts |
|---|------|---------|
| 1 | Build Record with all types | set int/double/bool/string, verify data_json() |
| 2 | Nested Record | set("roi", Record().set("x",10)), read back |
| 3 | Array push | push("items", Record()), verify size |
| 4 | **operator[] chaining** | `r["a"]["b"].as_int(42)` on present + missing |
| 5 | **Path expression "a.b.c"** | dot-separated navigation |
| 6 | **Path expression "a[0].b"** | array index + object key |
| 7 | **Path missing key → default** | `r["x.y.z"].as_int(99)` → 99 |
| 8 | **Path empty string** | `r[""].as_int(0)` → 0, no crash |
| 9 | **Path key > 256 chars** | verify truncation or correct handling |
| 10 | **Path "[-1]"** | doesn't crash, returns default |
| 11 | **Path "[999]" out of bounds** | returns default |
| 12 | **image_keys_json with special chars** | key containing `"` doesn't break JSON |
| 13 | **Copy semantics** | Record copy, modify copy, original unchanged |
| 14 | **Move semantics** | Record move, source empty |
| 15 | **as_record() deep copy** | modify extracted record, original unchanged |
| 16 | Image in Record | image("k", img), get_image("k"), verify pixels |

### test_image_pool.cpp (NEW)

| # | Test | Asserts |
|---|------|---------|
| 1 | Create + data + release | handle != 0, data not null, pixels writable |
| 2 | Addref + double release | refcount 1→2, release→1, release→0 (freed) |
| 3 | **Release twice (double-free guard)** | second release is a no-op, no crash |
| 4 | **Data after release returns null** | handle is gone |
| 5 | **Concurrent create from 4 threads** | no corruption, all handles unique |
| 6 | **Concurrent addref/release from 4 threads** | final refcount correct |
| 7 | **Width/height/channels correct** | after create, query matches |
| 8 | **from_image + to_image round-trip** | pixels identical |
| 9 | **Shard distribution** | 16 consecutive handles hit different shards |
| 10 | **Large image (20MP)** | create 60MB image, write/read, release |

### test_ops.cpp (NEW)

| # | Test | Asserts |
|---|------|---------|
| 1 | toGray white → 255 | single white pixel RGB → 255 gray |
| 2 | toGray black → 0 | single black pixel → 0 |
| 3 | toGray already gray → same | passthrough |
| 4 | threshold at boundary | value == t → 0, value == t+1 → 255 |
| 5 | threshold invert (via custom) | verify correct polarity |
| 6 | invert 0 → 255, 255 → 0 | pixel correctness |
| 7 | invert round-trip | invert(invert(img)) == img |
| 8 | sobel flat image → all zeros | no edges = no output |
| 9 | sobel single edge → nonzero | vertical line → horizontal gradient |
| 10 | erode shrinks white region | 3x3 white square erode(1) → 1x1 |
| 11 | dilate expands white region | 1x1 white pixel dilate(1) → 3x3 |
| 12 | countWhiteBlobs 0 blobs | all black → 0 |
| 13 | countWhiteBlobs 1 blob | single connected region → 1 |
| 14 | countWhiteBlobs 3 blobs | three separated dots → 3 |
| 15 | stats on known image | mean, stddev, min, max all correct |
| 16 | gaussian doesn't crash on 1x1 | edge case |
| 17 | boxBlur radius=0 → identity | output == input |

---

## Layer 2: Node Integration Tests

### ws_comprehensive.test.mjs (existing — expand)

| # | Test | Status |
|---|------|--------|
| 1 | Compile failure returns error | ✅ |
| 2 | Correct var values after run | ✅ |
| 3 | Run without script = warning | ✅ |
| 4 | cmd:run rejected during continuous | ✅ |
| 5 | Shutdown during continuous = clean exit | ✅ |
| 6 | Hot-reload during continuous | ⏭ skipped (DLL lock) |
| 7 | SEH crash → recovery → correct output | ✅ |
| 8 | set_param changes output | ✅ |
| 9 | Create instance + exchange | ✅ |
| 10 | list_params | ✅ |
| 11 | open_project restores instances | ✅ |
| 12 | get_project | ✅ |
| 13 | JPEG preview valid | ✅ |

### ws_state.test.mjs (NEW)

| # | Test | Asserts |
|---|------|---------|
| 1 | **state persists across runs** | compile use_demo, run 3x, verify run_count increments |
| 2 | **state survives hot-reload** | compile, run (state.count=1), recompile same script, run, verify count=2 not reset to 1 |
| 3 | **state survives param change** | set_param + run, verify state not cleared |
| 4 | **state with nested Record** | script stores `state().set("history", Record().push(...))`, verify structure preserved |

### ws_use.test.mjs (NEW)

| # | Test | Asserts |
|---|------|---------|
| 1 | **xi::use("cam0") grabs from backend instance** | create mock_camera instance, compile use_demo, run, verify input image var exists |
| 2 | **xi::use("det0") calls process** | create blob_analysis instance, compile script that uses det0, run, verify detection record |
| 3 | **xi::use exchange works** | script calls use("cam0").exchange(), verify round-trip |
| 4 | **xi::use on nonexistent instance** | script calls use("missing"), verify graceful failure (no crash) |

### ws_multifile.test.mjs (NEW)

| # | Test | Asserts |
|---|------|---------|
| 1 | **Two-file compilation** | main.cpp + helper.cpp, verify symbols resolve |
| 2 | **Include dir from project** | main.cpp includes "myheader.h" from custom include dir |
| 3 | **Compile error in second file** | bad syntax in helper.cpp → error reported, not silent |

### ws_commands.test.mjs (NEW — cover untested commands)

| # | Test | Asserts |
|---|------|---------|
| 1 | `ping` returns pong + timestamp | pong=true, ts is number |
| 2 | `version` returns semver + abi | version matches /\d+\.\d+\.\d+/ |
| 3 | `unload_script` + run → warning | unload then run = no crash |
| 4 | `list_instances` after create | instance appears in list |
| 5 | `set_instance_def` changes config | set new def, get_def shows change |
| 6 | `preview_instance` returns JPEG | start camera, preview, check JPEG magic |
| 7 | `process_instance` with source | create camera, start, process blob_analysis, verify blob_count |
| 8 | `save_project` / `load_project` round-trip | save, restart, load, verify params + instances |
| 9 | `get_plugin_ui` returns path | path exists on disk |
| 10 | Unknown command → error | "nope" → ok:false |

### ws_adversarial.test.mjs (NEW)

| # | Test | Asserts |
|---|------|---------|
| 1 | **Malformed JSON** | send garbage text → backend logs error, stays alive |
| 2 | **Missing cmd fields** | `{type:"cmd"}` (no id/name) → error, no crash |
| 3 | **Huge payload** | 1MB JSON string → rejected or handled, no OOM |
| 4 | **Rapid-fire commands** | 100 pings in < 100ms → all get responses |
| 5 | **Path injection in compile_and_load** | path with `&` → rejected by is_safe_path() |
| 6 | **Double start** | start, start again → second returns {already:true} or stops+restarts |
| 7 | **Double stop** | stop without start → ok (no crash) |
| 8 | **compile_and_load with empty path** | → error, not crash |

---

## Layer 3: E2E Tests

### index.cjs (existing — improve)

| # | Improvement |
|---|-------------|
| 1 | After run, verify vars contain expected names (not just screenshot) |
| 2 | Verify at least one image preview arrived in the viewer |

### full_pipeline.cjs (existing — improve)

| # | Improvement |
|---|-------------|
| 1 | After blob_analysis, verify blob_count > 0 in viewer data |
| 2 | Verify camera preview frame count > 0 during streaming |

---

## Test File → Bug Coverage Matrix

| Bug ID | Test File | Test # |
|--------|-----------|--------|
| A4-1 Command injection | ws_adversarial #5 | is_safe_path() |
| A2-6 Handle truncation | test_image_pool #5 | concurrent create |
| A3-1 Worker dangling ptrs | ws_comprehensive #6 | hot-reload during continuous |
| A3-7 Shutdown use-after-free | ws_comprehensive #5 | shutdown during continuous |
| A1-1 Future double-get | test_xi_core #5 | double get cached |
| A1-6 Negative image dims | test_xi_core #17 | clamped to 0 |
| A1-8 state() thread safety | ws_state #1-4 | (mutex documented, tested via sequential runs) |
| A3-2 cmd:run vs worker | ws_comprehensive #4 | rejected during continuous |
| A3-3 use_exchange no SEH | ws_use #3 | exchange round-trip |
| A2-2 Handle leak on crash | ws_crash (existing) | crash + recovery |
| A2-5 Plugin ctor ABI | ws_plugins (existing) | create_instance |
| A3-5 Output handle leak | test_image_pool #2-4 | refcount correctness |
| A1-7 const char* VAR | test_xi_core #10 | kind == String |
| A1-2 await_all void | test_xi_core #6 | void Future compiles |
| A1-4 image_keys_json | test_record #12 | special char escaping |
| A1-3 key truncation | test_record #9 | long key |
| A1-5 [-1] parse | test_record #10 | no crash |
| A4-2 WS fragmentation | ws_adversarial #3 | large payload |
| A4-5 JSON injection | test_record #12 | key escaping |

---

## Execution Plan

### Phase 1: C++ infrastructure (catches silent corruption bugs)
```
test_record.cpp        — 16 tests
test_image_pool.cpp    — 10 tests
test_ops.cpp           — 17 tests
test_xi_core.cpp       — add 6 tests to existing
```
Total: 49 new C++ assertions. Runtime: < 3 seconds.

### Phase 2: Node integration (catches protocol + lifecycle bugs)
```
ws_state.test.mjs      — 4 tests
ws_use.test.mjs         — 4 tests
ws_commands.test.mjs    — 10 tests
ws_adversarial.test.mjs — 8 tests
ws_multifile.test.mjs   — 3 tests
```
Total: 29 new Node tests. Runtime: ~2 minutes.

### Phase 3: E2E improvements
```
index.cjs              — 2 improvements
full_pipeline.cjs      — 2 improvements
```

### Final coverage target

```
After Phase 1+2:
xi_async/Future:    ████████░░  80%
xi_var/VAR:         ████████░░  80%
xi_param:           █████████░  90%
xi_record:          █████████░  90%
xi_ops:             ████████░░  80%
xi_state:           ████████░░  80%
xi_use:             ███████░░░  70%
ImagePool:          █████████░  90%
C ABI:              ██████░░░░  60%
WS protocol:        █████████░  90% (21 of 23 commands)
SEH:                █████████░  90%
Project lifecycle:  ████████░░  80%

Overall:            ████████░░  ~82%
```

---

## Run All Tests

```bash
# Phase 1: C++ (< 3 sec)
cd backend/build
./Release/test_xi_core.exe
./Release/test_protocol.exe
./Release/test_record.exe
./Release/test_image_pool.exe
./Release/test_ops.exe

# Phase 2: Node integration (sequential, ~2 min)
cd vscode-extension
for f in test/ws_*.test.mjs test/protocol.test.mjs; do
    node --test --test-timeout=600000 "$f"
done

# Phase 3: E2E (~1 min)
node test/runE2E.mjs

# Full pipeline with screenshots (~2 min)
node test/runPipeline.mjs
```
