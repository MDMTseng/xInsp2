cmake_minimum_required(VERSION 3.16)

# ---- Single source of truth for the project version --------------------
# Drives: cmd:version response, hello event, --version CLI, the SDK
# template's plugin.json, VS Code extension package.json (manually
# kept in sync), and the release zip filename.
set(XINSP2_VERSION "0.2.0")
project(xinsp2_backend VERSION ${XINSP2_VERSION} LANGUAGES C CXX)

# Embed git short hash if available (release builds will set XINSP2_COMMIT
# explicitly via -DXINSP2_COMMIT=...); fallback to "dev".
if(NOT DEFINED XINSP2_COMMIT)
    find_package(Git QUIET)
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
            OUTPUT_VARIABLE _git_hash
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_git_hash)
            set(XINSP2_COMMIT "${_git_hash}")
        endif()
    endif()
    if(NOT DEFINED XINSP2_COMMIT)
        set(XINSP2_COMMIT "dev")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8 /EHa)
else()
    add_compile_options(-Wall -Wextra)
endif()

# ---- Sanitizers (T0.1) --------------------------------------------------
# XINSP2_SANITIZE selects an instrumented build of the whole tree (tests +
# benches + binaries). Semicolon/comma list, e.g. -DXINSP2_SANITIZE=address or
# -DXINSP2_SANITIZE=address,undefined.
#
# TOOLCHAIN REALITY on this target (MSVC 19.50, VS 2026):
#   * address    — SUPPORTED. MSVC ships /fsanitize=address (ASan). Wired below.
#   * undefined  — NOT AVAILABLE on MSVC (no UBSan). Honored only under Clang/GCC;
#                  requesting it on MSVC is a hard configure error (no silent lie).
#   * thread     — NOT AVAILABLE. MSVC has no ThreadSanitizer, and clang-cl is not
#                  installed in this VS (only clang-format/clang-tidy ship). TSan
#                  is therefore impossible here; we hard-error rather than pretend.
#                  Race coverage instead comes from the high-iteration stress
#                  ctests (image_pool_stress_heavy / set_def_race_heavy), which can
#                  additionally be run under -DXINSP2_SANITIZE=address to catch the
#                  use-after-free / heap-corruption class that data races produce.
set(XINSP2_SANITIZE "" CACHE STRING
    "Sanitizers: comma/semicolon list of {address,undefined,thread} ('' = none)")
if(XINSP2_SANITIZE)
    string(REPLACE "," ";" _san_list "${XINSP2_SANITIZE}")
    list(REMOVE_DUPLICATES _san_list)
    set(XINSP2_SANITIZE_ADDRESS OFF)   # surfaced to the rest of the file
    foreach(_san IN LISTS _san_list)
        string(TOLOWER "${_san}" _san)
        if(MSVC)
            if(_san STREQUAL "address")
                set(XINSP2_SANITIZE_ADDRESS ON)
                add_compile_options(/fsanitize=address /Zi)
                # ASan is incompatible with the /RTC runtime checks (Debug) and
                # with incremental linking — strip/disable both so any config works.
                string(REGEX REPLACE "/RTC[1csu]*" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
                string(REGEX REPLACE "/RTC[1csu]*" "" CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")
                add_link_options(/INCREMENTAL:NO)
                message(STATUS "XINSP2_SANITIZE: MSVC AddressSanitizer (/fsanitize=address) enabled")
            elseif(_san STREQUAL "undefined")
                message(FATAL_ERROR "XINSP2_SANITIZE=undefined: MSVC has no UBSan. "
                    "Use a Clang/GCC build for UBSan, or drop 'undefined'.")
            elseif(_san STREQUAL "thread")
                message(FATAL_ERROR "XINSP2_SANITIZE=thread: MSVC has no ThreadSanitizer and "
                    "clang-cl is not installed in this VS. TSan is unavailable on this toolchain; "
                    "use the *_heavy stress ctests (optionally with XINSP2_SANITIZE=address) for race coverage.")
            else()
                message(FATAL_ERROR "XINSP2_SANITIZE: unknown sanitizer '${_san}'")
            endif()
        else()
            # Clang/GCC: address/undefined/thread all map to -fsanitize=.
            if(_san MATCHES "^(address|undefined|thread)$")
                add_compile_options(-fsanitize=${_san} -fno-omit-frame-pointer -g)
                add_link_options(-fsanitize=${_san})
                message(STATUS "XINSP2_SANITIZE: ${_san} sanitizer enabled (-fsanitize=${_san})")
            else()
                message(FATAL_ERROR "XINSP2_SANITIZE: unknown sanitizer '${_san}'")
            endif()
        endif()
    endforeach()
endif()

# OpenCV is mandatory (image operators); IPP / turbojpeg stay opt-in
# accelerators for the JPEG-encode path only.
option(XINSP2_HAS_IPP        "Link Intel IPP for JPEG encode acceleration" OFF)
option(XINSP2_HAS_TURBOJPEG  "Link libjpeg-turbo for direct SIMD JPEG encode" OFF)
# Default install root: C:\libjpeg-turbo64 (Windows) or /opt/libjpeg-turbo (Linux)
set(TURBOJPEG_ROOT "C:/libjpeg-turbo64" CACHE PATH "libjpeg-turbo install root")

# yyjson — the JSON DOM + serializer that replaces cJSON (v0.12.0, pinned).
# Mutable doc for build, immutable doc for fast read; pool allocator for
# zero-per-frame allocation. Linked into the backend + JIT-compiled plugins.
add_library(yyjson STATIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson/yyjson.c)
target_include_directories(yyjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson)

add_library(xi_core INTERFACE)
target_include_directories(xi_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)
# xi_record.hpp is backed by yyjson — every xi_core user links it.
target_link_libraries(xi_core INTERFACE yyjson)

# xi_fe — the FE-supervisor header surface (crash history/report, fe_status,
# respawn policy, BE-exit crash model). Lives in a SEPARATE include root
# (fe/include) so it is NOT on xi_core's path: the backend/runner binaries and
# the runtime script/plugin cl-compiler can't reach FE headers, which makes the
# core/supervisor split compile-enforced, not just cosmetic. Only the FE binary
# and the FE unit tests link this. yyjson comes in via the target's own link.
add_library(xi_fe INTERFACE)
target_include_directories(xi_fe INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../fe/include)
# xi.hpp / xi_plugin_support.hpp pull in <opencv2/opencv.hpp>, so every
# target that uses xi_core needs OpenCV's include + libs visible.
# OpenCV is configured immediately below.

enable_testing()

# Resolve optional encoder libs ONCE so every target that needs them
# (xinsp_backend, xinsp_runner, bench_jpeg) shares the same lookup.
# OpenCV: mandatory. Auto-detect typical Windows install locations if
# OpenCV_DIR isn't already set.
if(NOT DEFINED OpenCV_DIR AND NOT DEFINED ENV{OpenCV_DIR})
    foreach(_d "C:/opencv/opencv/build/x64/vc16/lib"
               "C:/opencv/opencv/build/x64/vc17/lib"
               "C:/opencv/build/x64/vc16/lib"
               "C:/opencv/build/x64/vc17/lib")
        if(EXISTS "${_d}/OpenCVConfig.cmake")
            set(OpenCV_DIR "${_d}")
            break()
        endif()
    endforeach()
endif()
find_package(OpenCV REQUIRED COMPONENTS core imgcodecs imgproc)
target_include_directories(xi_core INTERFACE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(xi_core INTERFACE ${OpenCV_LIBS})
if(XINSP2_HAS_IPP)
    # Default install layout: C:/Intel/ipp/<ver>/{include,lib,bin}
    # Override with -DIPP_ROOT=... or env IPP_ROOT.
    if(NOT DEFINED IPP_ROOT AND DEFINED ENV{IPP_ROOT})
        set(IPP_ROOT $ENV{IPP_ROOT})
    endif()
    if(NOT DEFINED IPP_ROOT)
        file(GLOB _ipp_versions "C:/Intel/ipp/*")
        foreach(_v ${_ipp_versions})
            if(EXISTS "${_v}/include/ippi.h")
                set(IPP_ROOT "${_v}")
                break()
            endif()
        endforeach()
    endif()
    find_path(IPP_INCLUDE_DIR ippi.h HINTS ${IPP_ROOT}/include)
    find_library(IPP_CORE_LIB ippcore HINTS ${IPP_ROOT}/lib)
    find_library(IPP_I_LIB    ippi    HINTS ${IPP_ROOT}/lib)
    find_library(IPP_CV_LIB   ippcv   HINTS ${IPP_ROOT}/lib)
    find_library(IPP_CC_LIB   ippcc   HINTS ${IPP_ROOT}/lib)
    if(NOT (IPP_INCLUDE_DIR AND IPP_CORE_LIB))
        message(WARNING "XINSP2_HAS_IPP=ON but IPP not found at ${IPP_ROOT}; set -DIPP_ROOT=...")
    endif()
endif()
if(XINSP2_HAS_TURBOJPEG)
    find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h
        HINTS ${TURBOJPEG_ROOT}/include /usr/include /usr/local/include)
    find_library(TURBOJPEG_LIB NAMES turbojpeg
        HINTS ${TURBOJPEG_ROOT}/lib /usr/lib /usr/local/lib)
    if(NOT (TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB))
        message(WARNING "XINSP2_HAS_TURBOJPEG=ON but libjpeg-turbo not found at ${TURBOJPEG_ROOT}")
    endif()
endif()

add_executable(test_xi_core tests/test_xi_core.cpp)
target_link_libraries(test_xi_core PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_xi_core PRIVATE pthread)
endif()
add_test(NAME xi_core COMMAND test_xi_core)

add_executable(test_record tests/test_record.cpp)
target_link_libraries(test_record PRIVATE xi_core)
add_test(NAME record COMMAND test_record)

# Canonical msgpack codec (xi/xi_mp.hpp) — the v3 pack-plane foundation. The
# header is zero-dependency (no yyjson, no OpenCV), so the unit test links
# nothing but the include dir. In-memory properties: writer determinism +
# canonical widths, reader round-trip, and hostile-input rejection.
add_executable(test_mp tests/test_mp.cpp)
target_include_directories(test_mp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME mp COMMAND test_mp)

# Canonical msgpack GOLDEN fixtures (protocol/fixtures/canonical) — the
# cross-implementation lockstep with the sibling TS/Python codecs. Drives the
# same Writer/canonicalizer and byte-compares against the committed *.bin;
# --regen rewrites them. Uses xi_json for the manifest, so it links yyjson.
add_executable(test_mp_fixtures tests/test_mp_fixtures.cpp)
target_link_libraries(test_mp_fixtures PRIVATE yyjson)
target_include_directories(test_mp_fixtures PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME mp_fixtures COMMAND test_mp_fixtures)
set_tests_properties(mp_fixtures PROPERTIES
    ENVIRONMENT "XINSP2_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/../protocol/fixtures")

# canonical_xcheck — the C++ leg of the three-way cross-language validation. It
# builds every portable vector in contract/canonical-vectors.json with the C++
# Writer and byte-compares to the expected hex (the SAME vectors the Python and
# TypeScript suites check), plus the recanon vectors. Byte agreement across the
# three implementations is the profile's cross-language proof. Labelled
# "contract" alongside the schema/baseline gates.
add_executable(test_canonical_xcheck tests/test_canonical_xcheck.cpp)
target_link_libraries(test_canonical_xcheck PRIVATE yyjson)
target_include_directories(test_canonical_xcheck PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME canonical_xcheck COMMAND test_canonical_xcheck)
set_tests_properties(canonical_xcheck PROPERTIES LABELS "contract"
    ENVIRONMENT "XINSP2_VECTORS=${CMAKE_CURRENT_SOURCE_DIR}/../contract/canonical-vectors.json")

# Plugin-manifest parse layer (xi_pm_parse.hpp): json_flag_true top-level-only (#22).
add_executable(test_pm_parse tests/test_pm_parse.cpp)
target_link_libraries(test_pm_parse PRIVATE xi_core)
add_test(NAME pm_parse COMMAND test_pm_parse)

add_executable(test_project_versioning tests/test_project_versioning.cpp)
target_link_libraries(test_project_versioning PRIVATE xi_core)
add_test(NAME project_versioning COMMAND test_project_versioning)

# Plugin load/reload teardown (#13 manifest-renamed, #9 compile:false external).
# A minimal real xi C-ABI plugin DLL + an in-process PluginManager driver.
add_library(test_teardown_plugin SHARED tests/test_teardown_plugin.cpp)
target_link_libraries(test_teardown_plugin PRIVATE xi_core)
add_executable(test_plugin_teardown tests/test_plugin_teardown.cpp)
target_link_libraries(test_plugin_teardown PRIVATE xi_core)
if(WIN32)
    target_link_libraries(test_plugin_teardown PRIVATE Ws2_32)
endif()
add_dependencies(test_plugin_teardown test_teardown_plugin)
target_compile_definitions(test_plugin_teardown PRIVATE
    TEST_PLUGIN_DLL="$<TARGET_FILE:test_teardown_plugin>")
add_test(NAME plugin_teardown COMMAND test_plugin_teardown)

# --- ABI Phase 0 safety nets (docs/internals/adr-001-host-api-freeze.md) ----
# Golden-plugin compat test: a minimal REAL xi C-ABI plugin pinned to ABI v9 /
# min-compat 6 (golden_plugin), loaded through the genuine plugin-load path
# (plugin_abi_compatible -> CAbiInstanceAdapter) and run once, asserting stable
# process() behaviour. THE single most important ABI safety net — source-built
# (not a committed blob), so it always tracks the live headers.
add_library(golden_plugin SHARED tests/golden_plugin.cpp)
target_link_libraries(golden_plugin PRIVATE xi_core)

# Phase 4 stale fixtures: deliberately-stale plugins reporting a PRE-v11 ABI
# (v9 and v10 — the old shm-bearing layout) from one source (STALE_PLUGIN_ABI).
# The golden test loads them through the real ABI gate and asserts the raised
# min-compat floor REFUSES them, proving the authorized break bites.
add_library(stale_plugin_v9 SHARED tests/stale_plugin.cpp)
target_compile_definitions(stale_plugin_v9 PRIVATE STALE_PLUGIN_ABI=9)
add_library(stale_plugin_v10 SHARED tests/stale_plugin.cpp)
target_compile_definitions(stale_plugin_v10 PRIVATE STALE_PLUGIN_ABI=10)

add_executable(test_golden_plugin tests/test_golden_plugin.cpp)
target_link_libraries(test_golden_plugin PRIVATE xi_core)
if(WIN32)
    target_link_libraries(test_golden_plugin PRIVATE Ws2_32)
endif()
add_dependencies(test_golden_plugin golden_plugin stale_plugin_v9 stale_plugin_v10)
target_compile_definitions(test_golden_plugin PRIVATE
    GOLDEN_PLUGIN_DLL="$<TARGET_FILE:golden_plugin>"
    STALE_V9_PLUGIN_DLL="$<TARGET_FILE:stale_plugin_v9>"
    STALE_V10_PLUGIN_DLL="$<TARGET_FILE:stale_plugin_v10>")
add_test(NAME golden_plugin COMMAND test_golden_plugin)

# --- OQ-7: static cross-plugin Record field contract ------------------------
# Opt-in producer/consumer schema declared via the optional xi_plugin_record_schema
# export (ABI-additive; xi_host_api untouched — see xi_record_schema.hpp). Four
# fixtures from one source (SCHEMA_ROLE) + a test that drives the real adapter
# load path and validates matched / wrong-type / missing-field pipelines.
add_library(schema_producer          SHARED tests/schema_fixture_plugin.cpp)
target_compile_definitions(schema_producer          PRIVATE SCHEMA_ROLE=0)
add_library(schema_consumer_ok       SHARED tests/schema_fixture_plugin.cpp)
target_compile_definitions(schema_consumer_ok       PRIVATE SCHEMA_ROLE=1)
add_library(schema_consumer_badtype  SHARED tests/schema_fixture_plugin.cpp)
target_compile_definitions(schema_consumer_badtype  PRIVATE SCHEMA_ROLE=2)
add_library(schema_consumer_missing  SHARED tests/schema_fixture_plugin.cpp)
target_compile_definitions(schema_consumer_missing  PRIVATE SCHEMA_ROLE=3)
foreach(t schema_producer schema_consumer_ok schema_consumer_badtype schema_consumer_missing)
    target_link_libraries(${t} PRIVATE xi_core)
endforeach()

add_executable(test_record_schema tests/test_record_schema.cpp)
target_link_libraries(test_record_schema PRIVATE xi_core)
if(WIN32)
    target_link_libraries(test_record_schema PRIVATE Ws2_32)
endif()
add_dependencies(test_record_schema
    schema_producer schema_consumer_ok schema_consumer_badtype schema_consumer_missing)
target_compile_definitions(test_record_schema PRIVATE
    PRODUCER_DLL="$<TARGET_FILE:schema_producer>"
    CONSUMER_OK_DLL="$<TARGET_FILE:schema_consumer_ok>"
    CONSUMER_BADTYPE_DLL="$<TARGET_FILE:schema_consumer_badtype>"
    CONSUMER_MISSING_DLL="$<TARGET_FILE:schema_consumer_missing>")
add_test(NAME record_schema COMMAND test_record_schema)

# B2 — a plugin operator that throws must not unwind across the extern "C" ABI
# boundary. XI_PLUGIN_IMPL wraps every per-call export in try/catch (defense-in-
# depth for prebuilt / cross-CRT plugins where such an unwind is UB). The fixture
# throws from every operator; the test drives the raw exports + the real adapter
# and asserts each returns a safe sentinel with no terminate.
add_library(throwing_plugin SHARED tests/throwing_plugin.cpp)
target_link_libraries(throwing_plugin PRIVATE xi_core)
add_executable(test_plugin_exception tests/test_plugin_exception.cpp)
target_link_libraries(test_plugin_exception PRIVATE xi_core)
if(WIN32)
    target_link_libraries(test_plugin_exception PRIVATE Ws2_32)
endif()
add_dependencies(test_plugin_exception throwing_plugin)
target_compile_definitions(test_plugin_exception PRIVATE
    THROWING_DLL="$<TARGET_FILE:throwing_plugin>")
add_test(NAME plugin_exception COMMAND test_plugin_exception)

# Post-fault quarantine policy (adoption-map item 14): a controllable-crash plugin
# (raw exports, so a fault reaches the host SEH boundary) drives the REAL adapter
# reinit/refuse mechanics + the health overlay the fault boundary sets.
add_library(fault_plugin SHARED tests/fault_plugin.cpp)
target_link_libraries(fault_plugin PRIVATE xi_core)
add_executable(test_fault_policy tests/test_fault_policy.cpp)
target_link_libraries(test_fault_policy PRIVATE xi_core)
if(WIN32)
    target_link_libraries(test_fault_policy PRIVATE Ws2_32)
endif()
add_dependencies(test_fault_policy fault_plugin)
target_compile_definitions(test_fault_policy PRIVATE
    FAULT_PLUGIN_DLL="$<TARGET_FILE:fault_plugin>")
add_test(NAME fault_policy COMMAND test_fault_policy)

# Capability plane pilot (docs/new_gen/14): host registry + forwarding funnel +
# reentrancy guard, driven against a REAL lib plugin (raw exports so a handler
# crash reaches the funnel's SEH boundary genuinely). Covers register/resolve/
# call round-trip, the name-only registry's $v/$probe convention, unknown -1,
# crash -2 + quarantine charged to the lib instance, reentrancy -5 in both
# directions, lifecycle-only registration, and the unregister-on-destroy sweep.
add_library(cap_test_plugin SHARED tests/cap_test_plugin.cpp)
target_link_libraries(cap_test_plugin PRIVATE xi_core)
add_executable(test_cap_plane tests/test_cap_plane.cpp)
target_link_libraries(test_cap_plane PRIVATE xi_core)
if(WIN32)
    target_link_libraries(test_cap_plane PRIVATE Ws2_32)
endif()
add_dependencies(test_cap_plane cap_test_plugin)
target_compile_definitions(test_cap_plane PRIVATE
    CAP_TEST_PLUGIN_DLL="$<TARGET_FILE:cap_test_plugin>")
add_test(NAME cap_plane_test COMMAND test_cap_plane)

# Q0f — the deliberate per-crash doc leak is OBSERVABLE. Drives the real xi_use.hpp
# UseProxy (share_out reserves a ref; rc -2 leaks it) against the real crashing
# fault_plugin, and asserts DocRegistry::crash_leaked_lifetime (surfaced in
# dispatch_stats as crash_leaked_docs_lifetime) ticks once per caught crash while a
# host-owned doc genuinely stays stranded.
add_executable(test_crash_leak_counter tests/test_crash_leak_counter.cpp)
target_link_libraries(test_crash_leak_counter PRIVATE xi_core)
if(WIN32)
    target_link_libraries(test_crash_leak_counter PRIVATE Ws2_32)
endif()
add_dependencies(test_crash_leak_counter fault_plugin)
target_compile_definitions(test_crash_leak_counter PRIVATE
    FAULT_PLUGIN_DLL="$<TARGET_FILE:fault_plugin>")
add_test(NAME crash_leak_counter COMMAND test_crash_leak_counter)

# ABI freeze-signature guard: declares xi_host_api@9 frozen and fails the BUILD
# if its published layout changes (per-field offset + fn-pointer type, in order).
# Mostly compile-time static_assert; the test target makes it part of the build.
add_executable(test_abi_freeze tests/test_abi_freeze.cpp)
target_link_libraries(test_abi_freeze PRIVATE xi_core)
add_test(NAME abi_freeze COMMAND test_abi_freeze)

# >>> host-api-segregation Phase 2 (xi.preview carve) — localized append block.
# Proves the carved xi.preview@1 interface (resolved via host->get_interface)
# compresses byte-for-byte identically to the legacy compress_image field, and
# that the SDK xi::Plugin::compress() wrapper resolves-then-caches with a
# legacy-field fallback. Kept self-contained to ease merge with sibling appends.
add_executable(test_preview_interface tests/test_preview_interface.cpp)
target_link_libraries(test_preview_interface PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_preview_interface PRIVATE pthread)
endif()
add_test(NAME preview_interface COMMAND test_preview_interface)
# <<< host-api-segregation Phase 2 end.

# >>> host-api-segregation Phase 3 (xi.imaging/doc/emit/log carve) — localized block.
# Proves each remaining domain interface resolves via host->get_interface and is
# byte-for-byte identical to the legacy xi_host_api fields it groups, and that the
# SDK xi::Plugin wrappers resolve-then-cache with a legacy-field fallback on a
# simulated pre-v10 host. Kept self-contained to ease merge with sibling appends.
add_executable(test_interface_domains tests/test_interface_domains.cpp)
target_link_libraries(test_interface_domains PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_interface_domains PRIVATE pthread)
endif()
add_test(NAME interface_domains COMMAND test_interface_domains)
# <<< host-api-segregation Phase 3 end.

# polaris2 wave-2: the xi.pack@1 carved data-plane door (xi_pack_abi.hpp) +
# dispatch dual-carry (TriggerEvent pack slot / TriggerBus::emit_pack). Host
# side only — the opaque-handle build/read accessors, refcount lifecycle with
# pooled-handle balance, and a Pack riding the same bus as a Record with no
# crosstalk. The plugin-side door + real end-to-end flow live in the plugin test.
add_executable(test_pack_door tests/test_pack_door.cpp)
target_link_libraries(test_pack_door PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_pack_door PRIVATE pthread)
endif()
add_test(NAME pack_door COMMAND test_pack_door)

# polaris2 gate P2: SCRIPT-SIDE Pack building (xi_script_pack.hpp). Host-role
# TU (the test_parallel_safety idiom: it owns the xi_use.hpp extern globals and
# injects the real host_api) driving xi::ScriptPackBuilder -> seal() ->
# ScriptPack: reads through the t.pack() surface, byte-level canonical-profile
# checks against xi::mp::Writer, the add_mp canonicalize gate (re-encode
# non-canonical, refuse malformed/ext), and builder/pack refcount balance.
add_executable(test_script_pack tests/test_script_pack.cpp)
target_link_libraries(test_script_pack PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_script_pack PRIVATE pthread)
endif()
add_test(NAME script_pack COMMAND test_script_pack)

add_executable(test_yyjson tests/test_yyjson.cpp)
target_link_libraries(test_yyjson PRIVATE yyjson)
target_include_directories(test_yyjson PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME yyjson COMMAND test_yyjson)

# γ pooled doc allocator (xi_doc_pool.hpp) — size-class free-list + yyjson round-trip.
add_executable(test_doc_pool tests/test_doc_pool.cpp)
target_link_libraries(test_doc_pool PRIVATE yyjson)
target_include_directories(test_doc_pool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME doc_pool COMMAND test_doc_pool)

# γ-4 host-side doc refcount (xi_doc_registry.hpp) — retain/release/free + leak watch.
add_executable(test_doc_registry tests/test_doc_registry.cpp)
target_link_libraries(test_doc_registry PRIVATE yyjson)
target_include_directories(test_doc_registry PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME doc_registry COMMAND test_doc_registry)

# OQ-7a observability export (xi_metrics.hpp) — frame counters + latency
# histogram. Drives record_frame from many threads and asserts counters/buckets
# advance and the JSON snapshot parses (yyjson). Heavy variant hammers the
# lock-free counters for the concurrency smoke.
add_executable(test_metrics tests/test_metrics.cpp)
target_link_libraries(test_metrics PRIVATE yyjson)
target_include_directories(test_metrics PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(test_metrics PRIVATE pthread)
endif()
add_test(NAME metrics COMMAND test_metrics)
add_test(NAME metrics_heavy COMMAND test_metrics)
set_tests_properties(metrics_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)

# Health/state contract (adoption-map item 10 / Bucket E): drives the header-only
# HealthRegistry state machine through its transitions + asserts the wire shape.
# Header-only like metrics, so it links only yyjson.
add_executable(test_health tests/test_health.cpp)
target_link_libraries(test_health PRIVATE yyjson)
target_include_directories(test_health PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(test_health PRIVATE pthread)
endif()
add_test(NAME health COMMAND test_health)

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE xi_core)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_diagnostics tests/test_diagnostics.cpp)
target_link_libraries(test_diagnostics PRIVATE xi_core)
add_test(NAME diagnostics COMMAND test_diagnostics)

# Blessed-concurrency compile guard: a raw `#pragma omp` in a SCRIPT's own source
# is rejected at the JIT boundary (routed to xi::parallel_for / xi::async); the
# scan + rejection fire before cl.exe, so this test needs no toolchain.
add_executable(test_omp_guard tests/test_omp_guard.cpp)
target_link_libraries(test_omp_guard PRIVATE xi_core)
add_test(NAME omp_guard COMMAND test_omp_guard)

# Crash-breadcrumb slot recycling: churn >>64 lifetime threads at bounded
# concurrency and prove per-thread slots are reclaimed on thread exit (attribution
# never degrades to the shared fallback slot 0).
add_executable(test_crash_breadcrumb tests/test_crash_breadcrumb.cpp)
target_link_libraries(test_crash_breadcrumb PRIVATE xi_core)
add_test(NAME crash_breadcrumb COMMAND test_crash_breadcrumb)

# Config/process race: an unprotected fixture plugin (race_probe) + a harness
# that hammers set_def against process() through the real CAbiInstanceAdapter,
# proving the CallScope gate serializes them for a non-reentrant plugin.
add_library(race_probe SHARED tests/race_probe.cpp)
target_include_directories(race_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(test_set_def_race tests/test_set_def_race.cpp)
target_link_libraries(test_set_def_race PRIVATE xi_core)
target_compile_definitions(test_set_def_race PRIVATE RACE_PROBE_DLL="race_probe.dll")
add_dependencies(test_set_def_race race_probe)
add_test(NAME set_def_race COMMAND test_set_def_race
         WORKING_DIRECTORY $<TARGET_FILE_DIR:test_set_def_race>)

# ABI v7 prepare/commit gating: a fixture (stage_probe) that parks process() in
# the cap=1 slot, proving the adapter calls prepare() UNGATED (completes while a
# process is in flight) but commit() GATED (blocks until the slot frees).
add_library(stage_probe SHARED tests/stage_probe.cpp)
target_include_directories(stage_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(test_prepare_concurrency tests/test_prepare_concurrency.cpp)
target_link_libraries(test_prepare_concurrency PRIVATE xi_core)
target_compile_definitions(test_prepare_concurrency PRIVATE STAGE_PROBE_DLL="stage_probe.dll")
add_dependencies(test_prepare_concurrency stage_probe)
add_test(NAME prepare_concurrency COMMAND test_prepare_concurrency
         WORKING_DIRECTORY $<TARGET_FILE_DIR:test_prepare_concurrency>)

# G3.2 debug lifecycle-contract asserts: a fixture (reentry_probe) whose process()
# re-enters the real CAbiInstanceAdapter on the host thread, proving the Debug-only
# guard TRAPS same-thread re-entry into a non-reentrant instance (process→set_def /
# →commit / →process) while leaving the legal cross-thread + reentrant paths clean.
add_library(reentry_probe SHARED tests/reentry_probe.cpp)
target_include_directories(reentry_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(test_lifecycle_asserts tests/test_lifecycle_asserts.cpp)
target_link_libraries(test_lifecycle_asserts PRIVATE xi_core)
target_compile_definitions(test_lifecycle_asserts PRIVATE REENTRY_PROBE_DLL="reentry_probe.dll")
add_dependencies(test_lifecycle_asserts reentry_probe)
add_test(NAME lifecycle_asserts COMMAND test_lifecycle_asserts
         WORKING_DIRECTORY $<TARGET_FILE_DIR:test_lifecycle_asserts>)

# SHM / worker-process / script-runner tests removed 2026-05 with the
# FE/BE in-process split (no more SHM region or worker processes).
set_tests_properties(protocol PROPERTIES
    ENVIRONMENT "XINSP2_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/../protocol/fixtures"
)

add_executable(test_image_pool tests/test_image_pool.cpp)
target_link_libraries(test_image_pool PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_image_pool PRIVATE pthread)
endif()
add_test(NAME image_pool COMMAND test_image_pool)

# v3 keyed-buffer Pack container (polaris2 wave-1, xi_pack.hpp): lifecycle,
# O(1) offset index, seal/immutability, pooled-handle balance vs ImagePool
# stats, mixed small/large frames, and the drop-on-crash release story.
add_executable(test_xi_pack tests/test_xi_pack.cpp)
target_link_libraries(test_xi_pack PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_xi_pack PRIVATE pthread)
endif()
add_test(NAME xi_pack COMMAND test_xi_pack)

# Ingress canonicalizer (xi_ingress.hpp) — the domain edge (task 1c). The ONLY
# path from foreign/untrusted msgpack bytes to a Pack entry: structural
# validation + profile normalization (NaN flatten, dup/non-string-key reject) +
# the optional semantic seam, and the pool-handle ext rule (never imported).
add_executable(test_ingress tests/test_ingress.cpp)
target_link_libraries(test_ingress PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_ingress PRIVATE pthread)
endif()
add_test(NAME ingress COMMAND test_ingress)

# Parallel-region safety regression tests (Part I hazards A/B/C — trigger
# snapshot, OpenMP/SEH containment, worker-thread image-owner propagation).
# Built WITH /openmp so xi::parallel_for exercises the REAL multi-threaded
# OpenMP path (per-omp-thread SEH translator + owner re-install), not its serial
# #else fallback. /EHa (project-wide) lets _set_se_translator convert the SEH
# fault into a catchable xi::seh_exception.
add_executable(test_parallel_safety tests/test_parallel_safety.cpp)
target_link_libraries(test_parallel_safety PRIVATE xi_core)
if(MSVC)
    target_compile_options(test_parallel_safety PRIVATE /openmp)
else()
    target_compile_options(test_parallel_safety PRIVATE -fopenmp)
    target_link_libraries(test_parallel_safety PRIVATE pthread)
    target_link_options(test_parallel_safety PRIVATE -fopenmp)
endif()
add_test(NAME parallel_safety COMMAND test_parallel_safety)

# High-stress race nets for the owner-propagation + watchdog epoch-cancel GAPs
# (§28 Tier-2/3). Same /openmp requirement as parallel_safety so parallel_for
# hits the real threaded owner-reinstall path. The _heavy variant scales the
# contention; run under -DXINSP2_SANITIZE=address for the UAF/corruption subset.
add_executable(test_owner_cancel_stress tests/test_owner_cancel_stress.cpp)
target_link_libraries(test_owner_cancel_stress PRIVATE xi_core)
if(MSVC)
    target_compile_options(test_owner_cancel_stress PRIVATE /openmp)
else()
    target_compile_options(test_owner_cancel_stress PRIVATE -fopenmp)
    target_link_libraries(test_owner_cancel_stress PRIVATE pthread)
    target_link_options(test_owner_cancel_stress PRIVATE -fopenmp)
endif()
add_test(NAME owner_cancel_stress COMMAND test_owner_cancel_stress)
add_test(NAME owner_cancel_stress_heavy COMMAND test_owner_cancel_stress)
set_tests_properties(owner_cancel_stress_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)

add_executable(test_image_pool_stress tests/test_image_pool_stress.cpp)
target_link_libraries(test_image_pool_stress PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_image_pool_stress PRIVATE pthread)
endif()
add_test(NAME image_pool_stress COMMAND test_image_pool_stress)

# High-iteration race-stress (T0.1, race coverage). Same binaries, but
# XINSP2_STRESS_SCALE multiplies the churn/ABA/refcount/CallScope hammering. On
# MSVC (no TSan) this is the systematic-ish net; run it under
# -DXINSP2_SANITIZE=address to also trap the UAF/heap-corruption class a race
# produces. Labeled "stress" so it can be selected/excluded (ctest -L stress).
add_test(NAME image_pool_stress_heavy COMMAND test_image_pool_stress)
set_tests_properties(image_pool_stress_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)
add_test(NAME set_def_race_heavy COMMAND test_set_def_race
         WORKING_DIRECTORY $<TARGET_FILE_DIR:test_set_def_race>)
set_tests_properties(set_def_race_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)

# WS teardown race (external review 08 finding 2). Sender threads hammer
# send_text while a client repeatedly connects+disconnects, forcing the poll
# thread through close_client() concurrent with in-flight sends — the exact
# close-off-lock / fd-reuse window. Header-only server; Winsock is linked by the
# header's #pragma comment(lib) on MSVC. Heavy variant scales connect rounds.
add_executable(test_ws_teardown_race tests/test_ws_teardown_race.cpp)
target_include_directories(test_ws_teardown_race PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(test_ws_teardown_race PRIVATE pthread)
endif()
add_test(NAME ws_teardown_race COMMAND test_ws_teardown_race)
add_test(NAME ws_teardown_race_heavy COMMAND test_ws_teardown_race)
set_tests_properties(ws_teardown_race_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)

# WS exclusive-bind hardening (polaris2/exclusive-bind). Proves the two
# properties of swapping SO_REUSEADDR → SO_EXCLUSIVEADDRUSE on the Windows listen
# socket: (a) hijack prevention — a rival SO_REUSEADDR bind on the live
# listener's port now FAILS; (b) rapid respawn — a start→stop→start cycle on one
# fixed port still succeeds every time (FE BE-respawn must not regress). Heavy
# variant scales respawn cycles.
add_executable(test_ws_exclusive_bind tests/test_ws_exclusive_bind.cpp)
target_include_directories(test_ws_exclusive_bind PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(test_ws_exclusive_bind PRIVATE pthread)
endif()
add_test(NAME ws_exclusive_bind COMMAND test_ws_exclusive_bind)
add_test(NAME ws_exclusive_bind_heavy COMMAND test_ws_exclusive_bind)
set_tests_properties(ws_exclusive_bind_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)

# Ordered-emit gate probe (§28 Tier-1 GAP: dispatch queue + EmitGate). Header-only
# primitive (xi_emit_gate.hpp) so it links nothing but the test itself; exercises
# the parallel-compute/ordered-emit invariant, the dtor backstop, and the
# stop-wakes-waiters teardown path. Heavy variant scales the frame count.
add_executable(test_emit_gate tests/test_emit_gate.cpp)
target_include_directories(test_emit_gate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(NOT MSVC)
    target_link_libraries(test_emit_gate PRIVATE pthread)
endif()
add_test(NAME emit_gate COMMAND test_emit_gate)
add_test(NAME emit_gate_heavy COMMAND test_emit_gate)
set_tests_properties(emit_gate_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)

# Hot-reload g_script swap probe (§28 Tier-2 GAP). A fixture "script" DLL
# (reload_probe) is hot-reloaded through the REAL loader while reader threads
# snapshot g_script by value and call inspect() outside the lock — the exact
# run_one_inspection shape. Drives the module_lifetime deferred-FreeLibrary path;
# a stale-call UAF aborts via the fixture's magic check (ASan-visible). The writer
# cycles distinct on-disk copies so FreeLibrary genuinely unmaps.
add_library(reload_probe SHARED tests/reload_probe.cpp)
add_executable(test_hot_reload_swap tests/test_hot_reload_swap.cpp)
target_link_libraries(test_hot_reload_swap PRIVATE xi_core)
target_compile_definitions(test_hot_reload_swap PRIVATE RELOAD_PROBE_DLL="reload_probe.dll")
add_dependencies(test_hot_reload_swap reload_probe)
add_test(NAME hot_reload_swap COMMAND test_hot_reload_swap
         WORKING_DIRECTORY $<TARGET_FILE_DIR:test_hot_reload_swap>)
add_test(NAME hot_reload_swap_heavy COMMAND test_hot_reload_swap
         WORKING_DIRECTORY $<TARGET_FILE_DIR:test_hot_reload_swap>)
set_tests_properties(hot_reload_swap_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)

# G4 / OQ-5 — code_change-style state-migration hook. One fixture source
# (migrate_probe.cpp) is built three ways: a v1 "old" DLL, a v2 DLL that exports
# the migration hook, and a v2 DLL without it. The test drives the REAL loader +
# host decision helper (xi::script::migrate_state) to prove state migrates when
# the hook is present and DROPS (unchanged pre-G4 behaviour) when it is absent.
add_library(migrate_probe_v1        SHARED tests/migrate_probe.cpp)
target_compile_definitions(migrate_probe_v1        PRIVATE MIG_SCHEMA=1)
add_library(migrate_probe_v2        SHARED tests/migrate_probe.cpp)
target_compile_definitions(migrate_probe_v2        PRIVATE MIG_SCHEMA=2 MIG_HOOK)
add_library(migrate_probe_v2_nohook SHARED tests/migrate_probe.cpp)
target_compile_definitions(migrate_probe_v2_nohook PRIVATE MIG_SCHEMA=2)
add_executable(test_state_migrate tests/test_state_migrate.cpp)
target_link_libraries(test_state_migrate PRIVATE xi_core)
target_compile_definitions(test_state_migrate PRIVATE
    MIGRATE_PROBE_V1_DLL="migrate_probe_v1.dll"
    MIGRATE_PROBE_V2_DLL="migrate_probe_v2.dll"
    MIGRATE_PROBE_V2_NOHOOK_DLL="migrate_probe_v2_nohook.dll")
add_dependencies(test_state_migrate
    migrate_probe_v1 migrate_probe_v2 migrate_probe_v2_nohook)
add_test(NAME state_migrate COMMAND test_state_migrate
         WORKING_DIRECTORY $<TARGET_FILE_DIR:test_state_migrate>)

# U2 (docs/new_gen/16) — xi::Kv container unit test. Header-only under test
# (xi_kv.hpp = std + xi_mp.hpp), so like test_mp it links nothing.
add_executable(test_kv tests/test_kv.cpp)
target_include_directories(test_kv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME kv COMMAND test_kv)

# U2 — kv-channel hot-reload carry. The kv sibling of state_migrate above:
# one fixture source (kv_probe.cpp, exporting the REAL xi::detail::kv_*_thunk
# bodies over the real xi::kv() store) built three ways, driven through the
# REAL loader + xi::script::migrate_kv. SECTION E pins the doc-16 JSON-era
# self-seed pattern (Record channel restored -> script seeds kv -> kv carries).
add_library(kv_probe_v1        SHARED tests/kv_probe.cpp)
target_compile_definitions(kv_probe_v1        PRIVATE KVP_SCHEMA=1)
target_include_directories(kv_probe_v1        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(kv_probe_v2        SHARED tests/kv_probe.cpp)
target_compile_definitions(kv_probe_v2        PRIVATE KVP_SCHEMA=2 KVP_HOOK)
target_include_directories(kv_probe_v2        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_library(kv_probe_v2_nohook SHARED tests/kv_probe.cpp)
target_compile_definitions(kv_probe_v2_nohook PRIVATE KVP_SCHEMA=2)
target_include_directories(kv_probe_v2_nohook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(test_kv_migrate tests/test_kv_migrate.cpp)
target_link_libraries(test_kv_migrate PRIVATE xi_core)
target_compile_definitions(test_kv_migrate PRIVATE
    KV_PROBE_V1_DLL="kv_probe_v1.dll"
    KV_PROBE_V2_DLL="kv_probe_v2.dll"
    KV_PROBE_V2_NOHOOK_DLL="kv_probe_v2_nohook.dll")
add_dependencies(test_kv_migrate
    kv_probe_v1 kv_probe_v2 kv_probe_v2_nohook)
add_test(NAME kv_migrate COMMAND test_kv_migrate
         WORKING_DIRECTORY $<TARGET_FILE_DIR:test_kv_migrate>)

# ---- Perf-regression gate (T0.3) ---------------------------------------
# Promote the bench_* micro-benchmarks from "run manually" to ctest gates: each
# runs the bench in --gate mode, parses its integer timing metrics, and FAILS if
# any metric regresses past the threshold vs a committed per-machine baseline.
# Override the threshold with -DXINSP2_PERF_THRESHOLD_PCT=NN at configure time.
# The raw benches stay runnable standalone (no args = the usual verbose report).
set(XINSP2_PERF_THRESHOLD_PCT 25 CACHE STRING
    "Perf-gate regression threshold in percent (slower-is-worse)")
set(_perf_baseline_dir ${CMAKE_CURRENT_SOURCE_DIR}/tests/perf_baselines)
function(_xinsp_perf_gate test_name bench_target baseline_name)
    add_test(NAME ${test_name}
        COMMAND ${CMAKE_COMMAND}
            -DBENCH_EXE=$<TARGET_FILE:${bench_target}>
            -DBASELINE=${_perf_baseline_dir}/${baseline_name}
            -DTHRESHOLD_PCT=${XINSP2_PERF_THRESHOLD_PCT}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/perf_gate.cmake)
    # perf_gate.cmake prints "perf_gate: SKIP (...)" and exits 0 when the
    # committed baseline's environment fingerprint does not match this machine
    # (different hardware / JPEG backend / build, or a pre-fingerprint baseline)
    # — surface that as a ctest SKIP, not a silent pass.
    set_tests_properties(${test_name} PROPERTIES
        LABELS "perf" TIMEOUT 300
        SKIP_REGULAR_EXPRESSION "perf_gate: SKIP")
endfunction()

# Throughput micro-benchmark — also gated as `perf_image_pool` (see below).
add_executable(bench_image_pool tests/bench_image_pool.cpp)
target_link_libraries(bench_image_pool PRIVATE xi_core)
target_compile_definitions(bench_image_pool PRIVATE XINSP2_COMMIT="${XINSP2_COMMIT}")
if(NOT MSVC)
    target_link_libraries(bench_image_pool PRIVATE pthread)
endif()
_xinsp_perf_gate(perf_image_pool bench_image_pool image_pool.txt)

add_executable(test_sha256 tests/test_sha256.cpp)
target_link_libraries(test_sha256 PRIVATE xi_core)
add_test(NAME sha256 COMMAND test_sha256)

# QA team-loop units (FE/BE split hardening). See docs/design/fe-test-rounds/.
# qa_fault/qa_race pin the respawn cap + sliding-window math; qa_edge pins the
# crash-report parser (xi_crash_report.hpp) against crafted fixtures.
add_executable(test_qa_fault tests/test_qa_fault.cpp)
target_link_libraries(test_qa_fault PRIVATE xi_core xi_fe)
add_test(NAME qa_fault COMMAND test_qa_fault)

add_executable(test_qa_race tests/test_qa_race.cpp)
target_link_libraries(test_qa_race PRIVATE xi_core xi_fe)
add_test(NAME qa_race COMMAND test_qa_race)

add_executable(test_qa_edge tests/test_qa_edge.cpp)
target_link_libraries(test_qa_edge PRIVATE xi_core xi_fe)
add_test(NAME qa_edge COMMAND test_qa_edge)

# >>> lv2-capability-handshake (Part II Phase 3) — localized block.
# Proves the LV2-style required/optional capability handshake: plugin.json
# requires[]/optional[] parse into PluginInfo (xi_pm_parse.hpp), and
# plugin_caps_compatible (xi_cabi_adapter.hpp) refuses a plugin whose required
# interface the host's get_interface door does not publish at version >= min,
# surfacing a reason; optional ifaces never gate. Self-contained to ease merge
# with sibling appends.
add_executable(test_capability_handshake tests/test_capability_handshake.cpp)
target_link_libraries(test_capability_handshake PRIVATE xi_core)
if(NOT MSVC)
    target_link_libraries(test_capability_handshake PRIVATE pthread)
endif()
add_test(NAME capability_handshake COMMAND test_capability_handshake)
# <<< lv2-capability-handshake end.


add_executable(xinsp_backend
    src/service_main.cpp
    src/service_sinks.cpp
    src/service_inspect.cpp
    src/service_toolchain.cpp
    src/service_result.cpp
    src/service_dispatch.cpp
    src/service_cmd_lifecycle.cpp
    src/service_cmd_dispatch.cpp
    src/service_cmd_plugin.cpp
    src/service_cmd_project.cpp
    src/service_cmd_observability.cpp
    src/service_health.cpp
    src/stb_impl.cpp
    src/xi_image_io.cpp
)
target_link_libraries(xinsp_backend PRIVATE xi_core)
target_include_directories(xinsp_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(WIN32)
    target_link_libraries(xinsp_backend PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_backend PROPERTIES OUTPUT_NAME "xinsp-backend")
target_compile_definitions(xinsp_backend PRIVATE
    XINSP2_VERSION="${XINSP2_VERSION}"
    XINSP2_COMMIT="${XINSP2_COMMIT}")

# xinsp_worker / xinsp_script_runner / evil_worker removed 2026-05:
# the FE/BE in-process split means plugins and scripts run inside the
# backend, so there are no worker / script-runner processes to spawn.

# Headless production runner. Same libs as the server; no WebSocket.
add_executable(xinsp_runner src/runner_main.cpp src/stb_impl.cpp src/xi_image_io.cpp)
target_link_libraries(xinsp_runner PRIVATE xi_core)
target_include_directories(xinsp_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(WIN32)
    target_link_libraries(xinsp_runner PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_runner PROPERTIES OUTPUT_NAME "xinsp-runner")
target_compile_definitions(xinsp_runner PRIVATE
    XINSP2_VERSION="${XINSP2_VERSION}"
    XINSP2_COMMIT="${XINSP2_COMMIT}")

# Frontend supervisor. Spawns + monitors xinsp-backend.exe, records crash
# forensics on backend death, respawns it (rate-limited). (Driving the line to a
# safe state on BE death is a comms plugin's own sidecar, not the FE.) Links
# NEITHER xi_core NOR OpenCV — it only needs the portable xi_be_exit.hpp header
# + yyjson + the
# Win32 process/socket APIs, so it carries no accelerator-DLL dependency and is
# excluded from _xinsp_deploy_dlls below.
add_executable(xinsp_fe src/fe_main.cpp)
target_link_libraries(xinsp_fe PRIVATE yyjson xi_fe)
# backend/include for the shared core bits fe_main still uses (xi_clock);
# vendor for yyjson.h; xi_fe brings fe/include for the supervisor headers.
target_include_directories(xinsp_fe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(WIN32)
    target_link_libraries(xinsp_fe PRIVATE Ws2_32)
endif()
set_target_properties(xinsp_fe PROPERTIES OUTPUT_NAME "xinsp-fe")
target_compile_definitions(xinsp_fe PRIVATE
    XINSP2_VERSION="${XINSP2_VERSION}"
    XINSP2_COMMIT="${XINSP2_COMMIT}")

# (xinsp-comms gateway removed — PLC I/O is now a plugin concern; the BE-crash
# line-safe case is handled by a comms plugin's own sidecar process, not the
# core/FE. See docs/internals/comms-sidecar.md.)

# JPEG-encoder benchmark — quantifies the IPP/OpenCV/stb dispatch.
# Not run as a test (no pass/fail); invoke manually for numbers.
add_executable(bench_jpeg tests/bench_jpeg.cpp src/stb_impl.cpp)
target_link_libraries(bench_jpeg PRIVATE xi_core)
target_compile_definitions(bench_jpeg PRIVATE XINSP2_COMMIT="${XINSP2_COMMIT}")
if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR AND IPP_CORE_LIB)
    target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_IPP=1)
    target_include_directories(bench_jpeg PRIVATE ${IPP_INCLUDE_DIR})
    target_link_libraries(bench_jpeg PRIVATE
        ${IPP_CORE_LIB} ${IPP_I_LIB} ${IPP_CV_LIB} ${IPP_CC_LIB})
endif()
target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_OPENCV=1)
# OpenCV include + libs come via xi_core's INTERFACE.
if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB)
    target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_TURBOJPEG=1)
    target_include_directories(bench_jpeg PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(bench_jpeg PRIVATE ${TURBOJPEG_LIB})
endif()
_xinsp_perf_gate(perf_jpeg bench_jpeg jpeg.txt)

# Record in-process handoff micro-bench: the REAL pointer-adoption + image
# addref/release path a plugin call takes in-process (Record::share_out /
# adopt_shared, xi::Image pool addref), NOT a JSON serialize/parse round trip
# (that is only the layout-incompatible FALLBACK, kept exploratory in the bench).
#
# This bench needs ONLY the always-present vendored yyjson + the xi headers, so
# it builds on a clean checkout (no separately-fetched sources required) and is
# gated as `perf_record`. The bench-only serde_vendor sources (MPack/CWPack —
# see tests/serde_vendor/FETCH.md) are OPTIONAL: when present they are compiled
# in and enable an extra exploratory serde-floor comparison in the verbose
# report (guarded by __has_include / XI_BENCH_HAVE_SERDE in the source); when
# absent the bench still builds, gates, and runs — that comparison is skipped.
add_executable(bench_record tests/bench_record.cpp)
target_link_libraries(bench_record PRIVATE yyjson)
target_include_directories(bench_record PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson)
target_compile_definitions(bench_record PRIVATE XINSP2_COMMIT="${XINSP2_COMMIT}")
if(NOT MSVC)
    target_link_libraries(bench_record PRIVATE pthread)
endif()
# Optional serde-backend sources (MPack/CWPack) for the exploratory floor table.
set(_serde ${CMAKE_CURRENT_SOURCE_DIR}/tests/serde_vendor)
if(EXISTS ${_serde}/mpack/mpack.c AND EXISTS ${_serde}/cwpack/cwpack.c)
    target_sources(bench_record PRIVATE
        ${_serde}/mpack/mpack.c
        ${_serde}/cwpack/cwpack.c)
    target_include_directories(bench_record PRIVATE
        ${_serde}/mpack ${_serde}/cwpack)
endif()
# Gate it. Baselines require the xi.perf-baseline/1 fingerprint; an old baseline
# (or one keyed to the retired serialization metric) SKIPs-with-reason until it
# is recaptured on this machine with UPDATE_BASELINE=ON.
_xinsp_perf_gate(perf_record bench_record record.txt)

# Per-frame hot-path bench: the ACTUAL emit -> trigger bus -> dispatch lane ->
# tiny inspect -> ordered result span (05 review #6 — bench_record measured a
# data-layer op, NOT the per-frame pipeline). Links only xi_core (header-only):
# it drives the REAL TriggerBus/ImagePool/DocRegistry/EmitGate/MetricsRegistry and
# reproduces GroupLane's queue plumbing as MiniLane (that plumbing is welded into
# the backend exe behind the private service_internal.hpp). Gated as `perf_hotpath`
# on a closed-loop best-of MEDIAN latency; the verbose report (no args) also prints
# p50/p95/p99 + throughput + a burst/overload tail. Baseline requires the
# xi.perf-baseline/1 fingerprint, so it SKIPs-with-reason until recaptured on the
# gating machine — same pattern as perf_image_pool / perf_record.
add_executable(bench_hotpath tests/bench_hotpath.cpp)
target_link_libraries(bench_hotpath PRIVATE xi_core)
target_compile_definitions(bench_hotpath PRIVATE XINSP2_COMMIT="${XINSP2_COMMIT}")
if(NOT MSVC)
    target_link_libraries(bench_hotpath PRIVATE pthread)
endif()
_xinsp_perf_gate(perf_hotpath bench_hotpath hotpath.txt)

# Pack-vs-Record GO/NO-GO bench (docs/new_gen/08 §1d): the v3 uniform keyed-
# buffer plane (xi_pack.hpp + xi_mp.hpp) measured against the v2 shared Record
# doc on the SAME per-frame span as bench_hotpath — arena/contiguous build vs
# mutable-DOM alloc, memcpy hop vs doc-refcount CAS, O(1) sealed offset reads vs
# yyjson hash lookups, one-shot free vs registry reconciliation. Links xi_core
# (header-only) for the real ImagePool/DocRegistry/EmitGate/Pack/mp/Record.
# Gated as `perf_pack` on the metadata-micro medians + each lane's closed-loop
# single-worker p50; no baseline shipped, so it SKIPs-with-reason until captured
# on the gating machine — same pattern as perf_hotpath/perf_record.
add_executable(bench_pack tests/bench_pack.cpp)
target_link_libraries(bench_pack PRIVATE xi_core)
target_compile_definitions(bench_pack PRIVATE XINSP2_COMMIT="${XINSP2_COMMIT}")
if(NOT MSVC)
    target_link_libraries(bench_pack PRIVATE pthread)
endif()
_xinsp_perf_gate(perf_pack bench_pack pack.txt)

if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR AND IPP_CORE_LIB)
    target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_IPP=1)
    target_include_directories(xinsp_backend PRIVATE ${IPP_INCLUDE_DIR})
    target_link_libraries(xinsp_backend PRIVATE
        ${IPP_CORE_LIB} ${IPP_I_LIB} ${IPP_CV_LIB} ${IPP_CC_LIB})
endif()

target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_OPENCV=1)
# OpenCV include + libs come via xi_core's INTERFACE.

if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB)
    target_compile_definitions(xinsp_backend PRIVATE XINSP2_HAS_TURBOJPEG=1)
    target_include_directories(xinsp_backend PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(xinsp_backend PRIVATE ${TURBOJPEG_LIB})
endif()

# ---- Runtime DLL deployment --------------------------------------------
# Copy accelerator DLLs next to the exe so end users / CI don't need a
# pre-mangled PATH. We do this for xinsp_backend and xinsp_runner.
function(_xinsp_deploy_dlls tgt)
    set(_dll_files "")
    # Pre-built OpenCV ships its DLLs in <root>/x64/<vc>/bin.
    foreach(_vc "vc16" "vc17" "vc14")
        set(_bin "${OpenCV_DIR}/../bin")
        if(EXISTS "${_bin}")
            file(GLOB _cv_dlls "${_bin}/opencv_world*.dll" "${_bin}/opencv_videoio_*.dll")
            list(APPEND _dll_files ${_cv_dlls})
            break()
        endif()
    endforeach()
    if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_LIB)
        get_filename_component(_tj_dir "${TURBOJPEG_LIB}" DIRECTORY)
        get_filename_component(_tj_root "${_tj_dir}" DIRECTORY)
        if(EXISTS "${_tj_root}/bin/turbojpeg.dll")
            list(APPEND _dll_files "${_tj_root}/bin/turbojpeg.dll")
        endif()
    endif()
    if(XINSP2_HAS_IPP AND IPP_INCLUDE_DIR)
        get_filename_component(_ipp_root "${IPP_INCLUDE_DIR}" DIRECTORY)
        if(EXISTS "${_ipp_root}/bin")
            file(GLOB _ipp_dlls "${_ipp_root}/bin/ipp*.dll")
            list(APPEND _dll_files ${_ipp_dlls})
        endif()
    endif()
    foreach(_dll ${_dll_files})
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:${tgt}>/")
    endforeach()
endfunction()
_xinsp_deploy_dlls(xinsp_backend)
_xinsp_deploy_dlls(xinsp_runner)

# ---- ASan runtime deployment (T0.1) ------------------------------------
# An MSVC /fsanitize=address /MD build needs clang_rt.asan_dynamic-x86_64.dll
# (ships next to cl.exe) on the DLL search path at runtime. All test exes land
# in the same per-config output dir, so copying it next to one target makes it
# resolvable for the whole ctest suite without touching PATH.
if(XINSP2_SANITIZE_ADDRESS AND MSVC)
    get_filename_component(_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _asan_dlls "${_cl_dir}/clang_rt.asan_dynamic-x86_64.dll")
    if(_asan_dlls)
        add_custom_command(TARGET test_xi_core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_asan_dlls} "$<TARGET_FILE_DIR:test_xi_core>/")
        message(STATUS "XINSP2_SANITIZE: deploying ASan runtime ${_asan_dlls}")
    else()
        message(WARNING "XINSP2_SANITIZE=address: clang_rt.asan_dynamic-x86_64.dll "
            "not found next to ${CMAKE_CXX_COMPILER}; tests may fail to start.")
    endif()
endif()

# ============================================================================
# Part III G1 — scan/certification isolation (core_fix_plan.md §17-18 G1).
# APPENDED block (kept self-contained so it stays separable from sibling-agent
# appends). A crash fixture whose factory faults + a driver that is BOTH the
# test and the certify child (self-reexec via --certify-plugin): the good
# (golden) plugin certifies ok and registers; the bad one certifies `crashed`
# in a throwaway child and is skipped by scan_plugins.
# ============================================================================
add_library(certify_crash_plugin SHARED tests/certify_crash_plugin.cpp)
target_link_libraries(certify_crash_plugin PRIVATE xi_core)

add_executable(test_certify tests/test_certify.cpp)
target_link_libraries(test_certify PRIVATE xi_core)
if(WIN32)
    target_link_libraries(test_certify PRIVATE Ws2_32)
endif()
add_dependencies(test_certify certify_crash_plugin golden_plugin)
target_compile_definitions(test_certify PRIVATE
    CRASH_PLUGIN_DLL="$<TARGET_FILE:certify_crash_plugin>"
    GOLDEN_PLUGIN_DLL="$<TARGET_FILE:golden_plugin>")
add_test(NAME certify COMMAND test_certify)
# ============================================================================
# End Part III G1 block.
# ============================================================================

# ============================================================================
# Part III G2 — per-plugin crash attribution + auto-disable (core_fix_plan §18 G2).
# APPENDED block (kept self-contained so it stays separable from sibling-agent
# appends). Unit-pins the pure attribution + counter + quarantine-load-gate logic:
#   * crash_history_line/enrich culprit fields, PluginFaultTracker budget,
#   * the REAL scan_plugins quarantine gate (reusing G1's .xi_certify.json),
#   * un-quarantine + hash-gated auto-clear,
#   * an ASan/XINSP2_STRESS_SCALE race on the process-global culprit stamp.
# Reuses the golden_plugin DLL as a stand-in for a quarantined plugin folder.
# ============================================================================
add_executable(test_qa_quarantine tests/test_qa_quarantine.cpp)
target_link_libraries(test_qa_quarantine PRIVATE xi_core xi_fe)
if(WIN32)
    target_link_libraries(test_qa_quarantine PRIVATE Ws2_32)
endif()
add_dependencies(test_qa_quarantine golden_plugin)
target_compile_definitions(test_qa_quarantine PRIVATE
    GOLDEN_PLUGIN_DLL="$<TARGET_FILE:golden_plugin>")
add_test(NAME qa_quarantine COMMAND test_qa_quarantine)
# A heavier stress run of the culprit-stamp race (ASan build picks this up; TSan
# is unavailable on this box — OQ-1). Labelled "stress" like the sibling heavy
# variants so the normal suite stays fast.
add_test(NAME qa_quarantine_heavy COMMAND test_qa_quarantine)
set_tests_properties(qa_quarantine_heavy PROPERTIES
    LABELS "stress" ENVIRONMENT "XINSP2_STRESS_SCALE=8" TIMEOUT 300)
# ============================================================================
# End Part III G2 block.
# ============================================================================

# ---- Doc-coverage gate ------------------------------------------------------
# A freeze guard for DEVELOPER-FACING docs (the analogue of test_abi_freeze, for
# docs). Public surface — the plugin/script C-ABI exports the host resolves, the
# authoring XI_* macros, and the WS commands — kept shipping while docs/guides and
# docs/reference lagged (A4, G4/OQ-5, OQ-7a/b all landed in code + internal residue
# rounds before the guides were updated). This check derives that surface from the
# source of truth and fails if any item is undocumented in docs/guides|reference
# (internal residue + archive deliberately don't count) and not explicitly
# allow-listed. Pure-stdlib Python; if no interpreter is found the gate is skipped
# with a notice so a Python-less configure still succeeds.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME doc_coverage
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_doc_coverage.py)
    set_tests_properties(doc_coverage PROPERTIES LABELS "docs")
    # retired_terms: the mirror of doc_coverage — fails if a RETIRED API token
    # (VAR(/EMIT( macros, the legacy xi_inspect_entry(int) entry) appears on a
    # blessed copy-paste surface (docs/guides, docs/reference, overview, README,
    # the generated-project template in the VS Code extension) outside the
    # tools/.retired-terms-allow list. Same pure-stdlib Python, same "docs" label.
    add_test(NAME retired_terms
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_retired_terms.py)
    set_tests_properties(retired_terms PROPERTIES LABELS "docs")
    # contract_schema: the contract-first spike gate. Validates every schema under
    # contract/schemas/ against the constrained-subset meta-schema (way A) and
    # every wire fixture (protocol/fixtures + contract/examples) against its mapped
    # schema (way B). Needs the `jsonschema` package; validate.py self-skips (exit 0)
    # if it is absent, so a box without it still passes configure/build like the
    # doc_coverage gate. See contract/validate.py and docs/new_gen/05-schema-language-spike.md.
    add_test(NAME contract_schema
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/validate.py)
    set_tests_properties(contract_schema PROPERTIES LABELS "contract")
    # contract_baseline: the protocol-version gate. Diffs the live schemas under
    # contract/schemas/ against the committed shape snapshot in
    # contract/baseline/protocol-baseline.json and classifies the delta:
    # unchanged -> green; ADDITIVE -> fail, demanding a baseline refresh in the same
    # commit; BREAKING -> fail, demanding a refresh AND a protocol_version bump. This
    # is what makes the WS `abi` stamp real: the described wire shape can no longer
    # drift without the version moving with it. Pure stdlib (no jsonschema), so it
    # always runs when Python is present. See contract/baseline_gate.py, contract/README.md,
    # and docs/ext_review/06 finding 2.
    add_test(NAME contract_baseline
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/baseline_gate.py)
    set_tests_properties(contract_baseline PROPERTIES LABELS "contract")
    # canonical_xcheck_py — the PYTHON leg of the three-way cross-validation
    # (paired with the C++ canonical_xcheck above). Runs the canonical-codec
    # pytest suite, which checks the shared vectors AND consumes the C++ golden
    # fixtures (decode + recanonicalize -> byte-compare; hostile goldens must be
    # rejected). The runner self-skips (exit 0, loud SKIP) when pytest is not
    # installed for this interpreter, like the doc_coverage / contract_* gates.
    add_test(NAME canonical_xcheck_py
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/xinsp2_py/run_canonical_xcheck.py)
    set_tests_properties(canonical_xcheck_py PROPERTIES LABELS "contract")
    # contract_live: the THIRD contract leg (schemas <-> fixtures <-> LIVE bytes).
    # contract_schema proves the fixtures match the schemas; this proves the schemas
    # match what the REAL backend sends. It spawns backend/build/Release/xinsp-backend.exe
    # on an ephemeral port, drives a representative session (hello, dispatch_stats,
    # get_health, open/compile/run, start/stop -> health_changed, commit_group,
    # load_project, metrics), and validates every captured message against its schema
    # (events routed by their schema's type/name consts; rsps by originating command
    # via contract/live-wire-map.json; every rsp also envelope-checked vs rsp.schema.json).
    # Messages no schema describes are ratcheted against contract/live-allowlist.json:
    # an unlisted new message type FAILS. Self-skips (exit 0, loud "SKIP - NOT A PASS")
    # when the backend exe is not built or a Python dep is missing, like the doc_coverage /
    # contract_schema gates. LONGER timeout: the session does a cold cl.exe compile.
    # gate.py runs this ONCE, in its `live` stage (the gate's python +
    # XINSP2_REQUIRE_SCHEMA_GATE=1, so skipping is forbidden there) and excludes
    # this ctest duplicate (-E contract_live); the registration stays so a bare
    # `ctest` run remains complete on its own.
    # See contract/live_conformance.py and contract/README.md.
    add_test(NAME contract_live
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/live_conformance.py)
    set_tests_properties(contract_live PROPERTIES LABELS "contract" TIMEOUT 600)

    # codegen_equiv: the wave-3 contract-codegen equivalence + drift gate
    # (docs/new_gen/08 Wave 3; docs/new_gen/02 guard 4). Regenerates the
    # per-plugin artifacts from contract/plugins/*.decl.json into a tmp dir and
    # (a) byte-compares vs the committed generated/plugins/ (stale = fail);
    # (c) proves the generated _keys.gen.h, the decl, AND the hand-written
    # plugins/<p>/<p>_keys.h all carry the identical key-constant set + schema
    # version (drift either way = fail); (e) WARNs (non-fatal) for any plugin
    # shipping a hand-written *_keys.h with no decl (the coverage ratchet). Pure
    # stdlib, so like contract_baseline it runs whenever Python is present.
    add_test(NAME codegen_equiv
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/codegen/check_equiv.py)
    set_tests_properties(codegen_equiv PROPERTIES LABELS "contract")

    # codegen_equiv_compile: the COMPILED half of the same proof. A custom
    # command REGENERATES the _keys/_io/_schema headers from the decls into the
    # build tree; codegen_equiv_test then #includes those GENERATED headers in
    # place of the hand-written ones and drives the exact builder/extractor call
    # sites the plugin tests use -- a renamed constant or changed signature stops
    # it compiling (drop-in equivalence, proven at compile+run time). The output
    # Records are rebuilt from JSON exactly as the plugin tests do post-ABI, so no
    # plugin DLL is needed. See contract/codegen/equiv/test_codegen_equiv.cpp.
    set(CODEGEN_GEN_DIR ${CMAKE_BINARY_DIR}/codegen_gen)
    set(CODEGEN_GEN_HEADERS
        ${CODEGEN_GEN_DIR}/blob_analysis_keys.gen.h
        ${CODEGEN_GEN_DIR}/blob_analysis_io.gen.h
        ${CODEGEN_GEN_DIR}/blob_analysis_schema.gen.h
        ${CODEGEN_GEN_DIR}/mock_camera_keys.gen.h
        ${CODEGEN_GEN_DIR}/mock_camera_io.gen.h
        ${CODEGEN_GEN_DIR}/mock_camera_schema.gen.h
        ${CODEGEN_GEN_DIR}/config_swap_probe_keys.gen.h
        ${CODEGEN_GEN_DIR}/config_swap_probe_io.gen.h
        ${CODEGEN_GEN_DIR}/json_source_keys.gen.h
        ${CODEGEN_GEN_DIR}/json_source_io.gen.h
        ${CODEGEN_GEN_DIR}/synced_stereo_keys.gen.h
        ${CODEGEN_GEN_DIR}/synced_stereo_io.gen.h
        ${CODEGEN_GEN_DIR}/synced_stereo_schema.gen.h)
    add_custom_command(
        OUTPUT ${CODEGEN_GEN_HEADERS}
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/codegen/gen_contract.py
                --out ${CODEGEN_GEN_DIR}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../contract/codegen/gen_contract.py
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/plugins/blob_analysis.decl.json
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/plugins/mock_camera.decl.json
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/plugins/config_swap_probe.decl.json
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/plugins/json_source.decl.json
                ${CMAKE_CURRENT_SOURCE_DIR}/../contract/plugins/synced_stereo.decl.json
        COMMENT "codegen: regenerating contract headers into ${CODEGEN_GEN_DIR}")
    add_custom_target(codegen_gen_headers DEPENDS ${CODEGEN_GEN_HEADERS})
    add_executable(codegen_equiv_test
        ${CMAKE_CURRENT_SOURCE_DIR}/../contract/codegen/equiv/test_codegen_equiv.cpp)
    add_dependencies(codegen_equiv_test codegen_gen_headers)
    target_include_directories(codegen_equiv_test PRIVATE ${CODEGEN_GEN_DIR})
    target_link_libraries(codegen_equiv_test PRIVATE xi_core)
    add_test(NAME codegen_equiv_compile COMMAND codegen_equiv_test)
    set_tests_properties(codegen_equiv_compile PROPERTIES LABELS "contract")
else()
    message(STATUS "doc_coverage ctest skipped: no Python3 interpreter found")
endif()

# ---- Release compat-manifest self-check ------------------------------------
# The release artifact carries a machine-readable compat-manifest.json (built by
# tools/compat_manifest.mjs from the real version sources). This gate builds +
# validates that manifest WITHOUT cutting a full release: every field resolves
# (no nulls), versions are semver, the ABI/WS numbers match the headers read
# independently, and tools/compat-matrix.json still matches the per-package
# version sources. Wired here so a version-constant move breaks LOUDLY instead of
# silently producing a null-field manifest in a shipped zip. Node-guarded; skipped
# with a notice on a box without node, mirroring the Python gates above.
find_program(NODE_EXECUTABLE NAMES node node.exe)
if(NODE_EXECUTABLE)
    add_test(NAME compat_manifest
        COMMAND ${NODE_EXECUTABLE} --test
                ${CMAKE_CURRENT_SOURCE_DIR}/../tools/compat_manifest.test.mjs)
    set_tests_properties(compat_manifest PROPERTIES LABELS "release")
else()
    message(STATUS "compat_manifest ctest skipped: no node interpreter found")
endif()

# ---- Script self-sufficiency compile guard ---------------------------------
# xi_script_support.hpp is FORCE-INCLUDED (/FI) into every inspection script, so
# it must be fully self-contained. A regression shipped once: dropping
# xi_instance.hpp from the xi.hpp umbrella left the header unable to see
# xi::InstanceRegistry, breaking every script compile — and the backend's own TUs
# did NOT catch it (they pull the header in from elsewhere). This OBJECT target
# includes ONLY xi_script_support.hpp (a bare script's view); if the header ever
# stops being self-sufficient it fails to compile. EXCLUDE_FROM_ALL + a ctest that
# builds it on demand, so normal builds aren't slowed but `ctest` catches the
# regression (compile-only — no link, guards header self-sufficiency directly).
add_library(script_selfcheck OBJECT EXCLUDE_FROM_ALL tests/script_selfcheck.cpp)
target_link_libraries(script_selfcheck PRIVATE xi_core)   # SDK + vendor + OpenCV include dirs
if(MSVC)
    target_compile_options(script_selfcheck PRIVATE /openmp)   # xi_script_support.hpp uses <omp.h>
else()
    target_compile_options(script_selfcheck PRIVATE -fopenmp)
endif()
add_test(NAME script_selfcheck
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target script_selfcheck --config $<CONFIG>)
set_tests_properties(script_selfcheck PROPERTIES LABELS "sdk")

# ---- Scaffold-template compile guard ---------------------------------------
# The sdk/templates/{easy,medium,expert} plugin sources are the copy-paste
# surface every new plugin author starts from — and they compile ONLY under the
# project-plugin force-include (/FIxi/xi_plugin_support.hpp), so the backend's
# own TUs never exercise them. They drifted before: a scaffold once taught the
# removed VAR() macro and shipped non-compiling code (ext review 11 P0). This
# guard renders each template's placeholders and compiles all three the way a
# scaffolded plugin is compiled; if any template stops compiling against the
# current SDK, the `template_selfcheck` ctest goes red. Same mechanism as
# script_selfcheck: OBJECT + EXCLUDE_FROM_ALL (compile-only, no link — three
# TUs each define the XI_PLUGIN_IMPL C exports, which only clash at link) built
# on demand by ctest, so normal builds aren't slowed.
set(_tpl_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/../sdk/templates)
set(_tpl_out_dir ${CMAKE_BINARY_DIR}/template_selfcheck)
set(_tpl_rendered "")
foreach(_tier easy medium expert)
    set(_in  ${_tpl_src_dir}/${_tier}/src/plugin.cpp)
    set(_out ${_tpl_out_dir}/${_tier}_plugin.cpp)
    file(READ ${_in} _tpl_body)
    # Substitute the same placeholders the scaffold renderer fills (render.mjs).
    string(REPLACE "{{CLASS}}" "TemplateSelfcheck_${_tier}" _tpl_body "${_tpl_body}")
    string(REPLACE "{{NAME}}"  "template_selfcheck_${_tier}" _tpl_body "${_tpl_body}")
    file(WRITE ${_out} "${_tpl_body}")
    list(APPEND _tpl_rendered ${_out})
    # Re-render whenever the template source changes (configure-time dependency).
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_in})
endforeach()
add_library(template_selfcheck OBJECT EXCLUDE_FROM_ALL ${_tpl_rendered})
target_link_libraries(template_selfcheck PRIVATE xi_core)   # SDK + yyjson + OpenCV include dirs
if(MSVC)
    # Mirror the plugin compile path: /EHa (host runs plugins under SEH
    # translation) + force-include the plugin support umbrella (xinsp2_plugin.cmake).
    target_compile_options(template_selfcheck PRIVATE /EHa /FIxi/xi_plugin_support.hpp)
else()
    target_compile_options(template_selfcheck PRIVATE -includexi/xi_plugin_support.hpp)
endif()
add_test(NAME template_selfcheck
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target template_selfcheck --config $<CONFIG>)
set_tests_properties(template_selfcheck PROPERTIES LABELS "sdk")

# ---- Examples compile gate -------------------------------------------------
# "Exemplars are tests" (adoption-map item 4). The examples/ script tree is the
# first thing a new user runs; once, 32 of its inspect.cpp files carried 181
# calls to the removed VAR() macro and none compiled on the current backend
# (review 11, F grade). This gate compiles EVERY example script the same way the
# backend JIT would — /FI xi/xi_script_support.hpp (the force-included support
# header), the OpenCV umbrella force-included first, /EHa /MD /std:c++20, /openmp
# — so a script that uses a removed/renamed API fails the build, not silently a
# user's first run. Compile-only OBJECT target (no link): it proves every TU
# COMPILES against the current headers, which is exactly the JIT's front-end.
#
# The source list is DERIVED (glob + CONFIGURE_DEPENDS) so a new example is
# gated automatically — no hand-maintained list to rot. It is every *.cpp under
# examples/ that is a SCRIPT: we exclude the co-located example PLUGINS
# (*/plugins/* — those force-include a DIFFERENT support header and are covered
# elsewhere), vendored third-party sources (*/vendor/*), and the standalone M0
# executable demo_async (its own add_executable, not a JIT script).
file(GLOB_RECURSE _examples_scripts CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/../examples/*.cpp)
list(FILTER _examples_scripts EXCLUDE REGEX "/plugins/")
list(FILTER _examples_scripts EXCLUDE REGEX "/vendor/")
list(FILTER _examples_scripts EXCLUDE REGEX "/demo_async\\.cpp$")
# qa_fault/badscript_project/inspect.cpp is a NEGATIVE fixture — it is
# deliberately uncompilable (the autostart compile-error / stay-up test). It
# must fail to compile, so it is intentionally NOT in this gate.
list(FILTER _examples_scripts EXCLUDE REGEX "/qa_fault/badscript_project/")
if(_examples_scripts)
    add_library(examples_gate OBJECT EXCLUDE_FROM_ALL ${_examples_scripts})
    target_link_libraries(examples_gate PRIVATE xi_core)   # SDK + vendor + OpenCV include dirs
    # One example (#include <extmath.h>) resolves a header from its own project
    # folder via project.json "include_dirs"; the JIT adds it as /I. Compile-only,
    # so only the header (not the .lib) is needed.
    target_include_directories(examples_gate PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../examples/script_external_dll/include)
    if(MSVC)
        # Mirror the JIT's script compile (xi_script_compiler.hpp): force-include
        # the OpenCV umbrella FIRST (via the PCH below, which CMake injects as a
        # forced include), then the script-support header, and build with /openmp
        # (the support header uses <omp.h>).
        target_compile_options(examples_gate PRIVATE
            /openmp /FIxi/xi_script_support.hpp)
        # PCH the OpenCV umbrella once (the dominant per-TU parse cost), shared
        # across all ~50 script TUs — same header the JIT PCHs. CMake force-injects
        # it ahead of the /FI above, matching the JIT's include order.
        target_precompile_headers(examples_gate PRIVATE <opencv2/opencv.hpp>)
        # Every script TU re-exports the force-included XI_SCRIPT_EXPORT thunks
        # (in the real JIT each script is its OWN DLL, so no clash). Bundling ~50
        # into one archive makes lib.exe warn LNK4006 "second definition ignored"
        # for each thunk — expected and harmless (we gate COMPILATION; a real
        # compile error still fails the per-TU cl.exe step before archiving).
        # Silence the noise so a genuine linker issue isn't buried.
        set_target_properties(examples_gate PROPERTIES STATIC_LIBRARY_OPTIONS "/IGNORE:4006")
    else()
        target_compile_options(examples_gate PRIVATE
            -fopenmp -include opencv2/opencv.hpp -include xi/xi_script_support.hpp)
    endif()
    add_test(NAME examples_gate
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
                --target examples_gate --config $<CONFIG>)
    set_tests_properties(examples_gate PROPERTIES LABELS "sdk")
endif()

# ============================================================================
# BEGIN fuzz/UBSan (clang-cl) — Cluster-2 / T1.1-T1.2 / OQ-1. Separable,
# agent-owned. Do not interleave with the blocks above. OFF by default: a plain
# MSVC configure never enters here, so the default build + ctest are unaffected.
# This is a SEPARATE build configuration from the MSVC XINSP2_SANITIZE block
# above (which is address-only on MSVC): it requires clang-cl (LLVM) and adds
# in-process libFuzzer targets + a real UBSan config MSVC cannot provide.
# Opt in with the 'fuzz' CMake preset, or -DXINSP2_FUZZ=ON plus a clang-cl
# compiler. See backend/tests/fuzz/README.md.
# ============================================================================
option(XINSP2_FUZZ "Build in-process libFuzzer + UBSan targets (requires clang-cl)" OFF)

if(XINSP2_FUZZ)
    if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
        message(FATAL_ERROR
            "XINSP2_FUZZ=ON requires clang-cl. Re-configure with "
            "-DCMAKE_CXX_COMPILER=\"C:/Program Files/LLVM/bin/clang-cl.exe\" "
            "(the 'fuzz' preset does this for you).")
    endif()
    add_subdirectory(tests/fuzz)
endif()
# ============================================================================
# END fuzz/UBSan (clang-cl)
# ============================================================================
