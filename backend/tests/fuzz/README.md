# In-process fuzzing (libFuzzer) + UBSan — backend

Coverage-guided, in-process fuzz harnesses for the parser / decode / ABI
boundaries the Python black-box WS smoke (`tests/fuzz/`, repo root) cannot reach.
These are **C++ libFuzzer targets** built with **clang-cl**, kept in a **separate
build configuration** from the default MSVC build/ctest.

## Why clang-cl

MSVC ships neither libFuzzer nor UBSan. LLVM/clang-cl does. The default MSVC
build is untouched: everything here is gated behind `-DXINSP2_FUZZ=ON`, which is
OFF by default, so a normal `cmake` + `ctest` never enters it.

clang-cl is **not on PATH** in a fresh shell — reference it by full path
(`C:/Program Files/LLVM/bin/clang-cl.exe`) or prepend `C:/Program Files/LLVM/bin`
to `PATH` in your build shell.

## Targets

| target                   | boundary under test                                        | header (read-only)     | links  |
|--------------------------|------------------------------------------------------------|------------------------|--------|
| `fuzz_parse_cmd`         | `xi::proto::parse_cmd` + `get_string_field/get_number_field` (WS `cmd` JSON) | `xi_protocol.hpp` | —      |
| `fuzz_yyjson`            | `yyjson_read` — the vendored parser behind every record decode | `yyjson.h`         | yyjson |
| `fuzz_record_json`       | `xi::Record::from_json_bytes` — the ABI `process()` JSON decode path | `xi_record.hpp`    | OpenCV |

The harnesses `#include` core headers **read-only**; they do not modify core.

## Build

Use the CMake presets (Ninja + clang-cl). From `backend/`:

```sh
export PATH="/c/Program Files/LLVM/bin:$PATH"    # git-bash; or use full paths

# libFuzzer + ASan (default, recommended)
cmake --preset fuzz
cmake --build build-fuzz --target fuzz_parse_cmd fuzz_yyjson fuzz_record_json
```

Or without presets:

```sh
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" \
  -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" \
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
  -DXINSP2_FUZZ=ON -DXINSP2_FUZZ_SANITIZER=address
cmake --build build-fuzz --target fuzz_parse_cmd
```

`XINSP2_FUZZ_SANITIZER` selects the sanitizer combined with libFuzzer:
`address` (default) · `undefined` · `address,undefined` · `none`.

> The fuzz build uses the **static CRT** (`/MT`, via `CMAKE_MSVC_RUNTIME_LIBRARY=
> MultiThreaded`) because the prebuilt `clang_rt.*` libs are static-CRT. ASan +
> the MSVC STL also needs `_DISABLE_STRING_ANNOTATION`/`_DISABLE_VECTOR_ANNOTATION`
> (set automatically) or lld-link fails with a `/failifmismatch` on
> `annotate_string`.

## Run

The ASan runtime is a DLL — put the clang runtime dir on `PATH` when running:

```sh
export PATH="/c/Program Files/LLVM/lib/clang/22/lib/windows:/c/Program Files/LLVM/bin:$PATH"

cd backend/tests/fuzz
B=../../build-fuzz/tests/fuzz

# grow the working corpus from the committed seeds; bounded budget
"$B/fuzz_parse_cmd.exe"   corpus/parse_cmd   seeds/parse_cmd   -max_total_time=60 -print_final_stats=1
"$B/fuzz_yyjson.exe"      corpus/yyjson      seeds/yyjson      -runs=2000000     -print_final_stats=1
"$B/fuzz_record_json.exe" corpus/record_json seeds/record_json -max_total_time=60 -print_final_stats=1
```

- `corpus/<target>/` is the **live working corpus** the fuzzer grows (gitignored).
- `seeds/<target>/` holds the committed starter inputs.
- On a finding, libFuzzer writes a `crash-<sha1>` reproducer to the cwd; replay it
  with `"$B/fuzz_parse_cmd.exe" crash-<sha1>`. Save it and REPORT — do not patch
  core here.

## UBSan configuration

Two ways to exercise UndefinedBehaviorSanitizer (MSVC cannot):

1. **UBSan unit tests** (mirrors P1a's MSVC `XINSP2_SANITIZE=address`, but for
   UBSan): pure socket-free unit tests rebuilt with `-fsanitize=undefined`,
   registered as ctests. Built by the `fuzz` preset automatically.
   ```sh
   ctest --test-dir build-fuzz -R ubsan_ --output-on-failure
   ```
2. **UBSan replay of the fuzzed boundaries**: the same harness bodies compiled
   `-fsanitize=undefined` and driven over a corpus by a standalone main (no
   libFuzzer — avoids a prebuilt-lib CRT limitation, see below).
   ```sh
   export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
   "$B/ubsan_replay_parse_cmd.exe"   corpus/parse_cmd
   "$B/ubsan_replay_record_json.exe" corpus/record_json
   "$B/ubsan_replay_yyjson.exe"      corpus/yyjson
   ```
   Point these at the ASan-fuzzer-grown `corpus/` for maximum path coverage.

There is also a `fuzz-ubsan` preset (`XINSP2_FUZZ_SANITIZER=undefined`). Note the
`fuzzer,undefined` **link** currently fails on this LLVM's prebuilt Windows libs
(`_stricmp` `__declspec(dllimport)` is only satisfied when ASan is also linked);
use `address` or `address,undefined` for combined fuzzer+sanitizer runs, and the
`ubsan_replay_*` targets above for UBSan over the fuzzed inputs.

## Notes / known gaps

- **`get_interface(id, version)` (v10 door resolver)** named in the task does not
  exist anywhere in the tree yet — no harness was written for it. Add one when the
  resolver lands.
- Do **not** build the whole tree under clang-cl (`cmake --build build-fuzz` with
  no target): non-fuzz targets like `xinsp_backend` / `bench_jpeg` use
  MSVC-specific constructs (`xi_jpeg.hpp`) that don't compile under clang-cl.
  Always name the fuzz/ubsan targets explicitly.
