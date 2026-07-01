# backend/tests/fuzz/CMakeLists.txt
#
# In-process, coverage-guided libFuzzer targets + a clang-cl UBSan build of a
# subset of the unit suite. This is a SEPARATE build configuration: it is only
# pulled in when -DXINSP2_FUZZ=ON (see the fenced block at the end of
# backend/CMakeLists.txt) and it REQUIRES clang-cl (LLVM). The default MSVC
# build/ctest never reaches this file.
#
# Sanitizer selection for the fuzz targets is XINSP2_FUZZ_SANITIZER:
#   address              -> -fsanitize=fuzzer,address        (default)
#   undefined            -> -fsanitize=fuzzer,undefined      (UBSan-under-fuzzer)
#   address,undefined    -> both
#   none                 -> -fsanitize=fuzzer                (plain, fastest)
#
# See README.md in this directory for how to build + run.

if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
    message(FATAL_ERROR
        "backend/tests/fuzz requires clang-cl. Configure with "
        "-DCMAKE_CXX_COMPILER=\"C:/Program Files/LLVM/bin/clang-cl.exe\" "
        "(or use the 'fuzz' CMake preset).")
endif()

set(XINSP2_FUZZ_SANITIZER "address" CACHE STRING
    "Sanitizer combined with libFuzzer: address | undefined | address,undefined | none")

# ---- Locate the clang compiler-rt runtime libraries -------------------------
# CMake's MSVC-like flow links by invoking lld-link DIRECTLY, so it never expands
# -fsanitize into the clang_rt runtime libs (fuzzer/ASan/UBSan) the way the
# clang-cl driver would. We therefore add those libs to the link line ourselves.
# Derive the runtime dir from the compiler location so an LLVM upgrade (new
# version subdir) keeps working.
get_filename_component(_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
file(GLOB _rt_dirs "${_llvm_bin}/../lib/clang/*/lib/windows")
if(NOT _rt_dirs)
    message(FATAL_ERROR "Could not find clang compiler-rt libs under ${_llvm_bin}/../lib/clang/*/lib/windows")
endif()
list(GET _rt_dirs 0 XI_RT)
message(STATUS "fuzz: compiler-rt dir = ${XI_RT}")

set(_rt_fuzzer     "${XI_RT}/clang_rt.fuzzer-x86_64.lib")
set(_rt_asan_dyn   "${XI_RT}/clang_rt.asan_dynamic-x86_64.lib")
set(_rt_asan_thunk "${XI_RT}/clang_rt.asan_static_runtime_thunk-x86_64.lib")
set(_rt_ubsan      "${XI_RT}/clang_rt.ubsan_standalone-x86_64.lib")
set(_rt_ubsan_cxx  "${XI_RT}/clang_rt.ubsan_standalone_cxx-x86_64.lib")

# Compile flags for the fuzz TUs (instrumentation), and the matching runtime
# libs for the (lld-link) link step.
if(XINSP2_FUZZ_SANITIZER STREQUAL "none")
    set(_fuzz_cflags -fsanitize=fuzzer)
else()
    set(_fuzz_cflags -fsanitize=fuzzer,${XINSP2_FUZZ_SANITIZER})
endif()

# The prebuilt clang_rt.* libs are STATIC-CRT (/MT); the fuzz build therefore
# uses CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded (set by the preset). We force-load
# the runtime libs with /wholearchive exactly as the clang-cl driver does, so the
# libFuzzer main + ASan/UBSan interceptors are all pulled in. These are linker
# FLAGS, so they go through target_link_options (NOT target_link_libraries, which
# would try to stat "/wholearchive:..." as a file).
set(_fuzz_link_opts "/wholearchive:${_rt_fuzzer}")
if(XINSP2_FUZZ_SANITIZER MATCHES "address")
    # ASan (which also backs UBSan's runtime) — pull interceptors + thunk.
    list(APPEND _fuzz_link_opts
        "/wholearchive:${_rt_asan_dyn}" "/wholearchive:${_rt_asan_thunk}")
else()
    # No ASan: libFuzzer's checks resolve against the standalone UBSan runtime.
    list(APPEND _fuzz_link_opts
        "/wholearchive:${_rt_ubsan}" "/wholearchive:${_rt_ubsan_cxx}")
endif()
if(XINSP2_FUZZ_SANITIZER MATCHES "undefined")
    list(APPEND _fuzz_cflags -fno-sanitize-recover=undefined)
endif()

# ASan + MSVC STL container annotations: the prebuilt clang_rt.fuzzer lib is
# built with annotations OFF, so our TUs must match or lld-link fails with a
# /failifmismatch on 'annotate_string'.
if(XINSP2_FUZZ_SANITIZER MATCHES "address")
    set(_fuzz_defs _DISABLE_STRING_ANNOTATION=1 _DISABLE_VECTOR_ANNOTATION=1)
else()
    set(_fuzz_defs)
endif()

# -------- fuzz targets --------------------------------------------------------
function(xi_fuzz_target name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_link_options(${name} PRIVATE ${_fuzz_link_opts})
    target_compile_options(${name} PRIVATE ${_fuzz_cflags})
    if(_fuzz_defs)
        target_compile_definitions(${name} PRIVATE ${_fuzz_defs})
    endif()
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        ${CMAKE_CURRENT_BINARY_DIR})
endfunction()

# parse_cmd + field pluckers — xi_protocol.hpp only (no OpenCV).
xi_fuzz_target(fuzz_parse_cmd  fuzz_parse_cmd.cpp  xi_core)
# raw yyjson decode path — vendored yyjson.c only (no OpenCV).
xi_fuzz_target(fuzz_yyjson     fuzz_yyjson.cpp     yyjson)
target_include_directories(fuzz_yyjson PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/yyjson ${CMAKE_SOURCE_DIR}/include)
# Record::from_json_bytes — the ABI process() JSON decode (links OpenCV via xi_core).
xi_fuzz_target(fuzz_record_json fuzz_record_json.cpp xi_core)

# -------- UBSan build of a subset of the unit suite --------------------------
# A clang-cl -fsanitize=undefined configuration of the pure (socket-free) unit
# tests, registered as ctests under this config. Mirrors P1a's MSVC
# XINSP2_SANITIZE=address opt-in, but for UndefinedBehaviorSanitizer, which MSVC
# cannot provide. New target names so the default MSVC targets are untouched.
option(XINSP2_FUZZ_UBSAN_TESTS "Also build UBSan copies of pure unit tests" ON)
if(XINSP2_FUZZ_UBSAN_TESTS)
    function(xi_ubsan_test name src)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE ${ARGN} ${_rt_ubsan} ${_rt_ubsan_cxx})
        target_compile_options(${name} PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
            ${CMAKE_CURRENT_BINARY_DIR})
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endfunction()

    xi_ubsan_test(ubsan_record   ${CMAKE_SOURCE_DIR}/tests/test_record.cpp   xi_core)
    xi_ubsan_test(ubsan_yyjson   ${CMAKE_SOURCE_DIR}/tests/test_yyjson.cpp   yyjson)
    target_include_directories(ubsan_yyjson PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor/yyjson ${CMAKE_SOURCE_DIR}/include)

    # UBSan replay of the FUZZED boundaries: the same harness bodies, compiled
    # -fsanitize=undefined and driven by ubsan_replay_main over a corpus (no
    # libFuzzer). Runs the parser/decoder paths the ASan fuzzer explored, now
    # under UBSan. Not auto-registered as ctests (they need a corpus argument);
    # invoke as: ubsan_replay_parse_cmd.exe corpus/parse_cmd  (see README).
    function(xi_ubsan_replay name harness_src)
        add_executable(${name} ${harness_src} ubsan_replay_main.cpp)
        target_link_libraries(${name} PRIVATE ${ARGN} ${_rt_ubsan} ${_rt_ubsan_cxx})
        target_compile_options(${name} PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
            ${CMAKE_CURRENT_BINARY_DIR})
    endfunction()
    xi_ubsan_replay(ubsan_replay_parse_cmd   fuzz_parse_cmd.cpp   xi_core)
    xi_ubsan_replay(ubsan_replay_record_json fuzz_record_json.cpp xi_core)
    xi_ubsan_replay(ubsan_replay_yyjson      fuzz_yyjson.cpp      yyjson)
    target_include_directories(ubsan_replay_yyjson PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor/yyjson ${CMAKE_SOURCE_DIR}/include)
endif()
