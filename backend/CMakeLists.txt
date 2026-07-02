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
# respawn policy, safe-state crash model). Lives in a SEPARATE include root
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

# Plugin-manifest parse layer (xi_pm_parse.hpp): json_flag_true top-level-only (#22).
add_executable(test_pm_parse tests/test_pm_parse.cpp)
target_link_libraries(test_pm_parse PRIVATE xi_core)
add_test(NAME pm_parse COMMAND test_pm_parse)

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

add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE xi_core)
add_test(NAME protocol COMMAND test_protocol)

add_executable(test_diagnostics tests/test_diagnostics.cpp)
target_link_libraries(test_diagnostics PRIVATE xi_core)
add_test(NAME diagnostics COMMAND test_diagnostics)

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
    src/service_inspect.cpp
    src/service_dispatch.cpp
    src/service_cmd_lifecycle.cpp
    src/service_cmd_dispatch.cpp
    src/service_cmd_plugin.cpp
    src/service_cmd_project.cpp
    src/service_cmd_observability.cpp
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

# Frontend supervisor. Spawns + monitors xinsp-backend.exe, drives the line to a
# safe state on backend death, respawns it (rate-limited). Links NEITHER xi_core
# NOR OpenCV — it only needs the portable xi_safe_state.hpp header + yyjson + the
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
else()
    message(STATUS "doc_coverage ctest skipped: no Python3 interpreter found")
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
