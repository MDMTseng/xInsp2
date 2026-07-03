cmake_minimum_required(VERSION 3.16)
project(xinsp2_plugins C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # /EHa (async exceptions) is REQUIRED, not cosmetic — it matches the SDK's own
    # plugin build (sdk/cmake/xinsp2_plugin.cmake) and the backend EXE. The host
    # runs every plugin call under _set_se_translator (xi_seh.hpp), and a source's
    # xi::spawn_worker installs the same translator on its worker thread; MSVC only
    # emits the unwind funclets that make that SEH→C++ translation sound AND run
    # local dtors during unwind under /EHa. Under the default /EHsc the catch still
    # fires but RAII in the faulting frame is skipped, leaking its pool_image
    # handles (1 ImagePool slot per absorbed crash) — and _set_se_translator warns
    # C4535 that it is inoperative.
    add_compile_options(/W3 /utf-8 /EHa)
endif()

# All plugin DLLs are namespaced with an "xi-" prefix (xi-<name>.dll). Set once
# here so every SHARED target below inherits it; each plugin.json "dll" matches.
set(CMAKE_SHARED_LIBRARY_PREFIX "xi-")

set(XI_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../backend/include)
set(XI_VENDOR  ${CMAKE_CURRENT_SOURCE_DIR}/../backend/vendor)

# Contract-codegen generated headers (contract/codegen/gen_contract.py). The
# wave-3 swap-in (docs/new_gen/08 Wave 3): every in-tree plugin's #include of its
# "<p>_keys.gen.h" / "<p>_io.gen.h" resolves here instead of a hand-written copy
# in the plugin dir, so the ONE committed generated set under version control is
# the single source of truth and its regenerate-diff shows in git. Directory scope
# so plugin DLL targets AND their contract-test exes below all see it. The decls
# live in contract/plugins/*.decl.json; `python contract/codegen/gen_contract.py`
# refreshes this dir; the codegen_equiv ctest fails a stale commit.
set(XI_CODEGEN ${CMAKE_CURRENT_SOURCE_DIR}/../contract/codegen/generated/plugins)
include_directories(${XI_CODEGEN})

# OpenCV — NO LONGER a mandatory dependency (OQ-9). xi_image.hpp / xi_abi.hpp
# are now OpenCV-free; the cv:: bridge lives in the opt-in <xi/xi_cv.hpp>. None
# of the in-tree plugins below do any CV, so they build with OpenCV absent.
# A plugin that wants cv:: just #includes <xi/xi_cv.hpp> and adds OpenCV to its
# own target (find_package(OpenCV ...) + target_link_libraries) — see the
# example plugins under examples/*/plugins for the pattern.

# yyjson — Record's DOM/codec and the only JSON lib plugins use. Every plugin
# includes xi_record (via xi_abi), so
# link + include it for ALL plugin targets defined below (directory scope).
add_library(yyjson_plugin STATIC ${XI_VENDOR}/yyjson/yyjson.c)
target_include_directories(yyjson_plugin PUBLIC ${XI_VENDOR}/yyjson)
include_directories(${XI_VENDOR}/yyjson)
link_libraries(yyjson_plugin)

# mock_camera
add_library(mock_camera SHARED mock_camera/mock_camera.cpp)
target_include_directories(mock_camera PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(mock_camera PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/mock_camera
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/mock_camera
)

# expose — the official script data-out surface (post-VAR): keeps the latest
# record per channel, JPEG-encodes images via the host cache + pushes the whole
# record as one atomic XEX1 binary frame for SUBSCRIBED channels. Script API:
# <xi/xi_expose.hpp>. See examples/preview_sink_demo.
add_library(expose SHARED expose/src/expose.cpp)
target_include_directories(expose PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(expose PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/expose
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/expose
)

# cache — buffers records in a ring and re-emits a buffered one on exchange
# (the plugin-side record/replay store; drives the hot-param re-inspect loop).
add_library(cache SHARED cache/src/cache.cpp)
target_include_directories(cache PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(cache PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/cache
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/cache
)

# config_swap_probe — reference plugin for the orchestrator config-swap design
# (double-slot prepare/commit via exchange; see
# docs/roadmap/config-bundles-and-orchestration.md)
add_library(config_swap_probe SHARED config_swap_probe/config_swap_probe.cpp)
target_include_directories(config_swap_probe PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(config_swap_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/config_swap_probe
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/config_swap_probe
)

# synced_stereo — emits left+right frames under one trigger ID
add_library(synced_stereo SHARED synced_stereo/synced_stereo.cpp)
target_include_directories(synced_stereo PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(synced_stereo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/synced_stereo
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/synced_stereo
)

# data_output
add_library(data_output SHARED data_output/data_output.cpp)
target_include_directories(data_output PRIVATE ${XI_INCLUDE})
set_target_properties(data_output PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/data_output
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/data_output
)

# blob_analysis
add_library(blob_analysis SHARED blob_analysis/blob_analysis.cpp)
target_include_directories(blob_analysis PRIVATE ${XI_INCLUDE} ${XI_VENDOR})

# json_source
add_library(json_source SHARED json_source/json_source.cpp)
target_include_directories(json_source PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(json_source PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/json_source
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/json_source
)

# record_save (uses stb_image_write for BMP output)
add_library(record_save SHARED
    record_save/record_save.cpp
    ../backend/src/stb_impl.cpp
)
# expose/src is on the include path for xex1_pack_dump.hpp + xex1_encode.hpp — the
# ONE shared pack->XEX1-v2 encoder record_save and expose both dump through (P3).
target_include_directories(record_save PRIVATE ${XI_INCLUDE} ${XI_VENDOR}
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
set_target_properties(record_save PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/record_save
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/record_save
)
set_target_properties(blob_analysis PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis
)

# --- Plugin data-contract tests (docs/new_gen/02-plugin-data-contract.md) ----
# Developer-side test exes: load the built plugin DLL via LoadLibrary and assert
# the contract behaviour (structured missing-input failures, schema skew, and the
# builder/extractor happy path). Plugins are trusted, so these are optional — but
# they keep the two most-copied exemplars honest. yyjson is already link_libraries'd
# for the whole directory (above), so the Record/Json code links in.
enable_testing()

# blob_analysis contract test — needs the host ImagePool (make_host_api) to build
# a real pool-backed gray input, so its process() runs exactly as in the backend.
add_executable(blob_analysis_test blob_analysis/tests/test_blob_analysis.cpp)
target_include_directories(blob_analysis_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis)
target_compile_definitions(blob_analysis_test PRIVATE
    BLOB_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis/xi-blob_analysis.dll")
add_dependencies(blob_analysis_test blob_analysis)
add_test(NAME blob_analysis_test COMMAND blob_analysis_test)

# polaris2 wave-2 pilot pair (docs/new_gen/08 Wave 2) — the xi.pack@1 door end
# to end against the REAL built DLLs: blob_analysis's pack-in/pack-out door
# (plugin-side capability probe), mock_camera's pack-mode emit through the bus,
# the chained flow, fail-loud, and pooled-handle balance. Needs both DLLs + a
# host ImagePool (make_host_api + install_pack_abi).
add_executable(pack_pilot_test pack_pilot_test.cpp)
target_include_directories(pack_pilot_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis
    ${CMAKE_CURRENT_SOURCE_DIR}/mock_camera)
target_compile_definitions(pack_pilot_test PRIVATE
    BLOB_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis/xi-blob_analysis.dll"
    MOCK_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mock_camera/xi-mock_camera.dll")
if(WIN32)
    target_link_libraries(pack_pilot_test PRIVATE Ws2_32)
endif()
add_dependencies(pack_pilot_test blob_analysis mock_camera)
add_test(NAME pack_pilot_test COMMAND pack_pilot_test)

# polaris2 Gate P2 (docs/new_gen/10) — script-side use()->pack-door CHAINING:
# the REAL xi_use.hpp UseProxy::process(ScriptPack) overload (this TU plays the
# script-DLL role and owns the extern callback globals, like the backend's
# test_crash_leak_counter) driven through a mirror of the service's
# use_pack_process_cb into blob_analysis's xi.pack@1 door. Also pins the
# t.pack() -> use().process chain, the $fault fail-loud contract, fail-closed
# edges (miss / no door / empty / older host), and pack+pool balance.
add_executable(use_pack_door_test use_pack_door_test.cpp)
target_include_directories(use_pack_door_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis)
target_compile_definitions(use_pack_door_test PRIVATE
    BLOB_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis/xi-blob_analysis.dll"
    MOCK_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mock_camera/xi-mock_camera.dll")
if(WIN32)
    target_link_libraries(use_pack_door_test PRIVATE Ws2_32)
endif()
add_dependencies(use_pack_door_test blob_analysis mock_camera)
add_test(NAME use_pack_door_test COMMAND use_pack_door_test)

# mock_camera contract test — config/command surface only (source plugin).
add_executable(mock_camera_test mock_camera/tests/test_mock_camera.cpp)
target_include_directories(mock_camera_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/mock_camera)
target_compile_definitions(mock_camera_test PRIVATE
    MOCK_CAMERA_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mock_camera/xi-mock_camera.dll")
add_dependencies(mock_camera_test mock_camera)
add_test(NAME mock_camera_test COMMAND mock_camera_test)

# synced_stereo contract test — gathering source. Drives config/command via the
# C ABI and captures a really-emitted stereo frame through a host emit_record
# sink (needs the ImagePool host), reading it back through the Frame extractor.
# polaris2 wave-2: also drives pack_mode ON and captures the sealed xi.pack@1
# Pack off the TriggerBus (install_pack_abi + install_trigger_hook), so it links
# Ws2_32 for the pack msgpack codec exactly like pack_pilot_test.
add_executable(synced_stereo_test synced_stereo/tests/test_synced_stereo.cpp)
target_include_directories(synced_stereo_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/synced_stereo)
target_compile_definitions(synced_stereo_test PRIVATE
    SYNCED_STEREO_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/synced_stereo/xi-synced_stereo.dll")
if(WIN32)
    target_link_libraries(synced_stereo_test PRIVATE Ws2_32)
endif()
add_dependencies(synced_stereo_test synced_stereo)
add_test(NAME synced_stereo_test COMMAND synced_stereo_test)

# json_source contract test — open output data plane, typed control surface
# (commands/config/patch). No images, so no pool host needed.
add_executable(json_source_test json_source/tests/test_json_source.cpp)
target_include_directories(json_source_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/json_source)
target_compile_definitions(json_source_test PRIVATE
    JSON_SOURCE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/json_source/xi-json_source.dll")
add_dependencies(json_source_test json_source)
add_test(NAME json_source_test COMMAND json_source_test)

# json_source BILINGUAL pack-mode test (polaris2 wave-2, docs/new_gen/10 Gate P1)
# — drives the REAL DLL through the xi.pack@1 door: pack_mode OFF leaves the
# Record path untouched (zero packs on the bus), ON emits sealed packs (scalars →
# canonical entries, nested → one ingress-canonicalized mp entry, + seq), and a
# depth-bomb document is rejected loudly with a $fault pack. Needs the host
# ImagePool + pack ABI + trigger bus (make_host_api + install_pack_abi), like
# pack_pilot_test.
add_executable(json_source_pack_test json_source/tests/test_json_source_pack.cpp)
target_include_directories(json_source_pack_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/json_source)
target_compile_definitions(json_source_pack_test PRIVATE
    JSON_SOURCE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/json_source/xi-json_source.dll")
if(WIN32)
    target_link_libraries(json_source_pack_test PRIVATE Ws2_32)
endif()
add_dependencies(json_source_pack_test json_source)
add_test(NAME json_source_pack_test COMMAND json_source_pack_test)

# config_swap_probe contract test — config/command/status + the prepare→commit
# double-slot (observable only through the Status extractor).
add_executable(config_swap_probe_test config_swap_probe/tests/test_config_swap_probe.cpp)
target_include_directories(config_swap_probe_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/config_swap_probe)
target_compile_definitions(config_swap_probe_test PRIVATE
    CONFIG_SWAP_PROBE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config_swap_probe/xi-config_swap_probe.dll")
add_dependencies(config_swap_probe_test config_swap_probe)
add_test(NAME config_swap_probe_test COMMAND config_swap_probe_test)

# config_swap_probe bilingual test (polaris2 wave-2, doc 10 gate P1) — the
# xi.pack@1 door end to end against the REAL built DLL through the host adapter:
# the capability probe, Record-vs-pack observable-state parity, unknown-entry
# tolerance, two-phase prepare/commit observability, and pooled/pack-registry
# balance. Needs a host ImagePool (make_host_api + install_pack_abi).
add_executable(config_swap_probe_pack_test config_swap_probe/tests/test_config_swap_probe_pack.cpp)
target_include_directories(config_swap_probe_pack_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/config_swap_probe)
target_compile_definitions(config_swap_probe_pack_test PRIVATE
    CONFIG_SWAP_PROBE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/config_swap_probe/xi-config_swap_probe.dll")
if(WIN32)
    target_link_libraries(config_swap_probe_pack_test PRIVATE Ws2_32)
endif()
add_dependencies(config_swap_probe_pack_test config_swap_probe)
add_test(NAME config_swap_probe_pack_test COMMAND config_swap_probe_pack_test)

# cache (BufferReplay) contract test — open buffered payload, typed command
# surface. Captures + replays through the ImagePool host emit sink.
add_executable(cache_test cache/tests/test_cache.cpp)
target_include_directories(cache_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/cache/src)
target_compile_definitions(cache_test PRIVATE
    CACHE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/cache/xi-cache.dll")
add_dependencies(cache_test cache)
add_test(NAME cache_test COMMAND cache_test)

# cache (BufferReplay) pack-door test (polaris2 wave-2, docs/new_gen/10 gate P1)
# — the xi.pack@1 side end to end against the REAL built DLL: pack retention
# across frames, eviction/clear/teardown release, zero-copy replay (same sealed
# handle, fresh id, stable pool live-count), and a mixed records+packs ring
# replayed in order. Needs the built cache DLL + a host ImagePool
# (make_host_api + install_pack_abi + install_trigger_hook) and the pack ABI.
add_executable(cache_pack_test cache/tests/test_cache_pack.cpp)
target_include_directories(cache_pack_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/cache/src)
target_compile_definitions(cache_pack_test PRIVATE
    CACHE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/cache/xi-cache.dll")
if(WIN32)
    target_link_libraries(cache_pack_test PRIVATE Ws2_32)
endif()
add_dependencies(cache_pack_test cache)
add_test(NAME cache_pack_test COMMAND cache_pack_test)

# XEX1 binary golden fixtures — drives the SAME encoder the expose plugin ships
# (expose/src/xex1_encode.hpp) and byte-compares its output against the committed
# protocol/fixtures/binary/*.bin. The JS + Python decoder tests decode those same
# goldens, so the three hand-rolled msgpack codecs are held in lockstep. Run with
# --regen to (re)write the goldens + manifest after an intentional frame change.
add_executable(xex1_fixtures_test expose/tests/test_xex1_fixtures.cpp)
target_include_directories(xex1_fixtures_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
add_test(NAME xex1_fixtures COMMAND xex1_fixtures_test)
set_tests_properties(xex1_fixtures PROPERTIES
    ENVIRONMENT "XINSP2_FIXTURES=${CMAKE_CURRENT_SOURCE_DIR}/../protocol/fixtures")

# polaris2 wave-2 step 3 — the memory ≈ wire ≈ disk HEADLINE (docs/new_gen/07 +
# 08 Wave 2): build a real xi::Pack, dump it to XEX1-v2, and assert the pack's
# in-memory canonical small plane equals the wire bytes equals a disk readback,
# byte-for-byte. Header-only (xi::Pack + xi::mp + the v2 encoder); no DLL load.
add_executable(xex1_v2_identity_test xex1_v2_identity_test.cpp)
target_include_directories(xex1_v2_identity_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
add_test(NAME xex1_v2_identity_test COMMAND xex1_v2_identity_test)

# polaris2 wave-2 step 3 — the GENERIC PROOF: the REAL expose DLL walks a sealed
# pack with zero producer knowledge and emits XEX1 (v2 opt-in / v1 default),
# captured off the host binary sink and decoded back. Needs the built expose DLL
# + a host ImagePool (make_host_api + install_pack_abi).
add_executable(expose_pack_test expose_pack_test.cpp)
target_include_directories(expose_pack_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
target_compile_definitions(expose_pack_test PRIVATE
    EXPOSE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/expose/xi-expose.dll")
if(WIN32)
    target_link_libraries(expose_pack_test PRIVATE Ws2_32)
endif()
add_dependencies(expose_pack_test expose)
add_test(NAME expose_pack_test COMMAND expose_pack_test)

# polaris2 P3 (docs/new_gen/10) — record_save persistence parity. Drives the REAL
# record_save DLL's pack door to persist a multi-entry pack (scalars + nested mp +
# image) as a .xex1 file, asserts the file bytes EQUAL the shared encoder's output
# (golden-style — proving record_save and expose dump byte-identically), then loads
# it back through the untrusted-disk ingress (xex1_pack_load.hpp) and asserts the
# rebuilt pack's entries / image pixels are identical and the image pool balances.
# Needs the built record_save DLL + a host ImagePool (make_host_api + install_pack_abi).
add_executable(record_save_pack_test record_save_pack_test.cpp)
target_include_directories(record_save_pack_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
target_compile_definitions(record_save_pack_test PRIVATE
    RECORD_SAVE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/record_save/xi-record_save.dll")
if(WIN32)
    target_link_libraries(record_save_pack_test PRIVATE Ws2_32)
endif()
add_dependencies(record_save_pack_test record_save)
add_test(NAME record_save_pack_test COMMAND record_save_pack_test)
