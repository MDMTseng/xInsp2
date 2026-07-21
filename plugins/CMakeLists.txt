cmake_minimum_required(VERSION 3.16)
project(xinsp2_plugins C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build accelerators (mirror of backend/CMakeLists): ccache caches compiles
# (active under Ninja; VS generator ignores the launcher), and lld-link swaps the
# MSVC linker for LLVM's faster drop-in — the COMPILER stays cl.exe (we point
# CMAKE_LINKER at lld-link via HINTS, never putting LLVM/bin on PATH). Both no-op if
# the tool is absent.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
endif()
if(MSVC)
    find_program(LLD_LINK_EXE lld-link
        HINTS "$ENV{ProgramW6432}/LLVM/bin" "$ENV{ProgramFiles}/LLVM/bin" "C:/Program Files/LLVM/bin")
    if(LLD_LINK_EXE)
        set(CMAKE_LINKER "${LLD_LINK_EXE}" CACHE FILEPATH "MSVC-compatible LLD linker" FORCE)
    endif()
endif()

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

# libjpeg-turbo — opt-in, MIRRORING backend/CMakeLists.txt. polaris2 ENCODE
# eviction: turbojpeg's direct SIMD JPEG encoder LEAVES the backend/core and
# lands in the imgcodec LIB PLUGIN (the xi.jpeg.encode provider). Same switch,
# same default (OFF; stb fallback), same install root as the backend so a build
# that opts the backend in opts imgcodec in too — perf parity for the host's
# compress_sink once it delegates to the capability. At v12 the backend target
# drops this define; here it stays as imgcodec's fast path.
# AUTO-DETECT libjpeg-turbo (mirrors backend/CMakeLists.txt): installed → default
# ON (the imgcodec preview encoder runs ~6x faster via direct SIMD tjCompress,
# doc 34); absent → OFF, falls back to OpenCV. No hard dependency. Sticky in cache;
# override with -DXINSP2_HAS_TURBOJPEG=ON/OFF.
set(TURBOJPEG_ROOT "C:/libjpeg-turbo64" CACHE PATH "libjpeg-turbo install root")
if(DEFINED XINSP2_HAS_TURBOJPEG)
    set(_xi_tj_default ${XINSP2_HAS_TURBOJPEG})
elseif(EXISTS "${TURBOJPEG_ROOT}/include/turbojpeg.h"
       OR EXISTS "/usr/include/turbojpeg.h"
       OR EXISTS "/usr/local/include/turbojpeg.h")
    set(_xi_tj_default ON)
else()
    set(_xi_tj_default OFF)
endif()
option(XINSP2_HAS_TURBOJPEG "Link libjpeg-turbo for direct SIMD JPEG encode in imgcodec" ${_xi_tj_default})
if(XINSP2_HAS_TURBOJPEG)
    find_path(TURBOJPEG_INCLUDE_DIR turbojpeg.h
        HINTS ${TURBOJPEG_ROOT}/include /usr/include /usr/local/include)
    find_library(TURBOJPEG_LIB NAMES turbojpeg
        HINTS ${TURBOJPEG_ROOT}/lib /usr/lib /usr/local/lib)
    if(NOT (TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB))
        message(WARNING "XINSP2_HAS_TURBOJPEG=ON but libjpeg-turbo not found at ${TURBOJPEG_ROOT}")
    endif()
endif()

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

# yyjson — the only JSON lib plugins use (config docs, json_source, the pack
# codec's canonical mp path). Link + include it for ALL plugin targets defined
# below (directory scope).
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

# controls_demo — def surface = the controls pluginlet (doc 37). Uses xi_json
# (yyjson) for the def marshalling and the pluginlets/ include root for
# <controls/controls.hpp>.
add_library(controls_demo SHARED controls_demo/controls_demo.cpp)
target_link_libraries(controls_demo PRIVATE yyjson_plugin)
target_include_directories(controls_demo PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${CMAKE_CURRENT_SOURCE_DIR}/../pluginlets)
set_target_properties(controls_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/controls_demo
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/controls_demo
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

# record_save (v12: the .xex1 pack dump is the sink; the .json/.bmp Record door
# was deleted, so stb_image_write is no longer linked)
add_library(record_save SHARED record_save/record_save.cpp)
# expose/src is on the include path for xex1_pack_dump.hpp + xex1_encode.hpp — the
# ONE shared pack->XEX1-v3 encoder record_save and expose both dump through (P3).
target_include_directories(record_save PRIVATE ${XI_INCLUDE} ${XI_VENDOR}
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
set_target_properties(record_save PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/record_save
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/record_save
)

# imgcodec — the FIRST OFFICIAL LIB PLUGIN (capability plane pilot, docs/
# new_gen/14). NO data plane: registers xi.jpeg.encode (content-keyed dedup
# memo cache — one encode serves every consumer) + xi.image.decode (stb_image,
# host read_image_file's designated v12 eviction target) via get_interface
# ("xi.cap.provider", 1). stb impls compiled in via backend/src/stb_impl.cpp
# (the record_save pattern).
add_library(imgcodec SHARED
    imgcodec/imgcodec.cpp
    ../backend/src/stb_impl.cpp
)
target_include_directories(imgcodec PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(imgcodec PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/imgcodec
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/imgcodec
)
# polaris2 ENCODE eviction: give imgcodec the SIMD JPEG encoder the backend used
# to own (xi::encode_jpeg's turbo path). Mirrors backend/CMakeLists.txt's wiring.
if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB)
    target_compile_definitions(imgcodec PRIVATE XINSP2_HAS_TURBOJPEG=1)
    target_include_directories(imgcodec PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(imgcodec PRIVATE ${TURBOJPEG_LIB})
    # The PLUGIN owns its native dependency: deploy turbojpeg.dll next to
    # xi-imgcodec.dll, NOT next to the backend exe. The runtime loader
    # (load_plugin_dll_, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) searches the plugin
    # DLL's own directory for its deps, so this resolves without the core exe
    # carrying the dependency — keeping the backend turbojpeg-free (v12 CUT).
    get_filename_component(_tj_dir "${TURBOJPEG_LIB}" DIRECTORY)
    get_filename_component(_tj_root "${_tj_dir}" DIRECTORY)
    if(EXISTS "${_tj_root}/bin/turbojpeg.dll")
        add_custom_command(TARGET imgcodec POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_tj_root}/bin/turbojpeg.dll" "$<TARGET_FILE_DIR:imgcodec>/")
    endif()
endif()

# ui_egress — the LIVE-UI egress service-middleware LIB PLUGIN (docs/new_gen/31).
# NO data plane: registers xi.ui.egress (push a preview -> latest-wins slot;
# own timer thread @ UI rate -> content-keyed dedup -> dispatch by descriptor 't'
# -> xi.jpeg.encode cap -> hand to expose). expose/src is on the include path for
# xex1_encode.hpp — the SHARED wire encoder egress builds the UI frame through.
add_library(ui_egress SHARED ui_egress/ui_egress.cpp)
target_include_directories(ui_egress PRIVATE ${XI_INCLUDE} ${XI_VENDOR}
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
set_target_properties(ui_egress PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/ui_egress
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/ui_egress
)

# lut_owner — the RESOURCE-HANDLE PATTERN demo lib plugin (doc 14 appendix
# "Resource-handle convention"): a TYPE-OWNER for demo.lut (immutable sorted
# i64->i64 lookup table). Heavy objects never ride packs — this DLL owns them
# in a ring of generation-leased slots; packs carry only the handle entry
# {type,id,gen,$v} as nested canonical mp. Registers demo.lut.build (content-
# deduped) / demo.lut.query / demo.lut.dump (the byte-deterministic persistence
# materializer) via get_interface("xi.cap.provider", 1).
add_library(lut_owner SHARED lut_owner/lut_owner.cpp)
target_include_directories(lut_owner PRIVATE ${XI_INCLUDE} ${XI_VENDOR})
set_target_properties(lut_owner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/lut_owner
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/lut_owner
)

# record_replay — the replay SOURCE, record_save's mirror (doc 10 gate P3): reads
# canonical .xex1 (XEX1-v3) dumps and emits each as a sealed Pack via the host
# xi.pack@1 door. expose/src is on the include path for xex1_pack_parse.hpp — the
# ONE untrusted-disk parse shared with the host-side loader (xex1_pack_load.hpp).
add_library(record_replay SHARED record_replay/record_replay.cpp)
target_include_directories(record_replay PRIVATE ${XI_INCLUDE} ${XI_VENDOR}
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
set_target_properties(record_replay PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/record_replay
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/record_replay
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
# for the whole directory (above), so the JSON code links in.
enable_testing()

# (v12: the Record-based blob_analysis_test was deleted with the Record data
# plane; the surviving coverage is the pack-door pilot pair below + the
# blob_analysis pack path exercised by pack_pilot_test / use_pack_door_test.)

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
# edges (miss / no door / empty / older host), mock_camera's CONTROL door
# (ex-feedback, 5c: json_source now seats the -4 door-less edge), and
# pack+pool balance.
add_executable(use_pack_door_test use_pack_door_test.cpp)
target_include_directories(use_pack_door_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis)
target_compile_definitions(use_pack_door_test PRIVATE
    BLOB_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/blob_analysis/xi-blob_analysis.dll"
    MOCK_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mock_camera/xi-mock_camera.dll"
    JSON_SOURCE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/json_source/xi-json_source.dll")
if(WIN32)
    target_link_libraries(use_pack_door_test PRIVATE Ws2_32)
endif()
add_dependencies(use_pack_door_test blob_analysis mock_camera json_source)
add_test(NAME use_pack_door_test COMMAND use_pack_door_test)

# (v12: the Record-based mock_camera_test / synced_stereo_test / json_source_test
# were deleted with the Record data plane. mock_camera + synced_stereo are now
# pack-only sources; json_source's pack-door coverage lives in
# json_source_pack_test + use_pack_door_test below.)

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

# (v12: the Record-based config_swap_probe_test was deleted with the Record data
# plane; the surviving coverage is the pack-door test below.)

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

# (v12: the Record-based cache_test was deleted with the Record data plane; the
# surviving coverage is the pack-door test below.)

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
# 08 Wave 2): build a real xi::Pack, dump it to XEX1-v3, and assert the pack's
# in-memory canonical small plane equals the wire bytes equals a disk readback,
# byte-for-byte. Header-only (xi::Pack + xi::mp + the v3 encoder); no DLL load.
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

# polaris2 gate P2 (docs/new_gen/10) — EXPOSE-FROM-SCRIPT: a script-held sealed
# pack goes through the REAL SDK surface (xi::ScriptPack + xi::use().push(), the
# TU playing the script DLL) into the REAL expose DLL's pack door, staged +
# flushed with the service's use_push_pack_cb discipline mirrored, and the
# pushed XEX1-v2 bytes are asserted BYTE-IDENTICAL to a direct host-side dump
# of the same pack. Needs the built expose DLL + a host ImagePool.
add_executable(expose_script_push_test expose_script_push_test.cpp)
target_include_directories(expose_script_push_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson)
target_compile_definitions(expose_script_push_test PRIVATE
    EXPOSE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/expose/xi-expose.dll")
if(WIN32)
    target_link_libraries(expose_script_push_test PRIVATE Ws2_32)
endif()
add_dependencies(expose_script_push_test expose)
add_test(NAME expose_script_push_test COMMAND expose_script_push_test)

# U3 (docs/new_gen/17) — frame-order delivery of staged pack pushes under
# PARALLEL dispatch, asserted at the sink's door: the REAL xi::EmitGate/EmitTurn
# ordering primitive + the REAL expose DLL door, the service's staged-push
# flush discipline mirrored per worker. Gated phase -> door-observed wire seq
# exactly in frame order (and in call order within a frame); ungated control on
# the same uneven workload -> inversions (proving the gate, not luck, ordered
# phase A); pack/pool registries balance.
add_executable(pack_order_gate_test pack_order_gate_test.cpp)
target_include_directories(pack_order_gate_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson)
target_compile_definitions(pack_order_gate_test PRIVATE
    EXPOSE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/expose/xi-expose.dll")
if(WIN32)
    target_link_libraries(pack_order_gate_test PRIVATE Ws2_32)
endif()
add_dependencies(pack_order_gate_test expose)
add_test(NAME pack_order_gate_test COMMAND pack_order_gate_test)

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

# polaris2 P3 — THE CLOSED LOOP (docs/new_gen/10 "persistence parity"): drives
# BOTH real DLLs — record_save persists a multi-entry pack as cap_000001.xex1,
# record_replay re-emits it as a sealed Pack off the TriggerBus — and asserts
# parity at the pack level (entry-for-entry, incl. restored $channel/$seq) AND
# the byte level (re-dump == disk == original's dump). Also the fail-loud edge:
# a corrupt file emits a sealed $fault pack and the cursor advances past it.
add_executable(record_replay_pack_test record_replay_pack_test.cpp)
target_include_directories(record_replay_pack_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson
    ${CMAKE_CURRENT_SOURCE_DIR}/expose/src)
target_compile_definitions(record_replay_pack_test PRIVATE
    RECORD_SAVE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/record_save/xi-record_save.dll"
    RECORD_REPLAY_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/record_replay/xi-record_replay.dll")
if(WIN32)
    target_link_libraries(record_replay_pack_test PRIVATE Ws2_32)
endif()
add_dependencies(record_replay_pack_test record_save record_replay)
add_test(NAME record_replay_pack_test COMMAND record_replay_pack_test)

# Capability plane pilot (docs/new_gen/14) — xi.imgcodec end to end against
# the REAL built DLL through the REAL host funnel: register-on-create, calls
# by name, THE DEDUP PROOF (two consumers + one sealed image = ONE encode,
# byte-identical JPEG out), decode round trip, $probe/$v convention, $fault
# contract answers, unregister-on-destroy, pack balance. Needs a host
# ImagePool (make_host_api + install_pack_abi + install_cap_plane).
add_executable(cap_imgcodec_test cap_imgcodec_test.cpp)
target_include_directories(cap_imgcodec_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson)
target_compile_definitions(cap_imgcodec_test PRIVATE
    IMGCODEC_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/imgcodec/xi-imgcodec.dll")
if(WIN32)
    target_link_libraries(cap_imgcodec_test PRIVATE Ws2_32)
endif()
add_dependencies(cap_imgcodec_test imgcodec)
add_test(NAME cap_imgcodec_test COMMAND cap_imgcodec_test)

# LIVE-UI egress pipeline (spec 31 / docs/internals/ui-egress.md) end to end
# across the THREE REAL DLLs through the REAL cap plane: push -> xi.ui.egress
# (ui_egress latest-wins slot + timer flusher) -> xi.jpeg.encode (imgcodec) ->
# xi.ui.sink (expose byte-blind store + subscription gate). Pins the LRU/slot
# runtime the python e2e cannot make deterministic — dedup, no-subscriber zero
# encode, latest-wins collapse, LRU eviction — observed only through the "stats"
# and "get"/"subscribe" exchanges. Needs the same host (make_host_api +
# install_pack_abi + install_cap_plane).
add_executable(cap_ui_egress_test cap_ui_egress_test.cpp)
target_include_directories(cap_ui_egress_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson)
target_compile_definitions(cap_ui_egress_test PRIVATE
    UI_EGRESS_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/ui_egress/xi-ui_egress.dll"
    IMGCODEC_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/imgcodec/xi-imgcodec.dll"
    EXPOSE_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/expose/xi-expose.dll")
if(WIN32)
    target_link_libraries(cap_ui_egress_test PRIVATE Ws2_32)
endif()
add_dependencies(cap_ui_egress_test ui_egress imgcodec expose)
add_test(NAME cap_ui_egress_test COMMAND cap_ui_egress_test)

# polaris2 CORE-CODEC EVICTION (stage 1) — the HOST's read_image_file
# (backend/src/xi_image_io.cpp, linked in so its static installer runs)
# delegating decode to xi.image.decode with an stb fallback: byte-equivalence
# across both engines (3-channel + native 2-channel gray+alpha), absent-
# capability / factory-timing fallback, self-serve reentrancy refusal, decoder-
# fault (quarantine) clean fallback + attribution, and identical fail modes on
# corrupt/null. Compiles the real host TU + stb impl into the EXE.
add_executable(cap_imgcodec_host_test
    cap_imgcodec_host_test.cpp
    ../backend/src/xi_image_io.cpp
    ../backend/src/stb_impl.cpp
)
target_include_directories(cap_imgcodec_host_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson)
target_compile_definitions(cap_imgcodec_host_test PRIVATE
    IMGCODEC_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/imgcodec/xi-imgcodec.dll")
if(WIN32)
    target_link_libraries(cap_imgcodec_host_test PRIVATE Ws2_32)
endif()
add_dependencies(cap_imgcodec_host_test imgcodec)
add_test(NAME cap_imgcodec_host_test COMMAND cap_imgcodec_host_test)

# polaris2 CORE-CODEC EVICTION (stage 2) — the HOST's preview JPEG encode
# (backend compress_sink) delegating to xi.jpeg.encode via encode_via_capability
# (backend/include/xi/xi_jpeg_cap.hpp), with the in-core xi::encode_jpeg fallback:
# byte-equivalence across both engines, dedup deferring to imgcodec (no host
# double-cache), quality plumbing, self-serve reentrancy refusal, encoder-fault
# (quarantine) clean fallback + attribution, and absent-capability in-core
# fallback. Compiles the in-core encoder + stb impl into the EXE; the turbojpeg
# opt-in matches imgcodec's so an opted-in build stays byte-equal engine-for-engine.
add_executable(cap_jpeg_encode_host_test
    cap_jpeg_encode_host_test.cpp
    ../backend/src/stb_impl.cpp
)
target_include_directories(cap_jpeg_encode_host_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson)
target_compile_definitions(cap_jpeg_encode_host_test PRIVATE
    IMGCODEC_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/imgcodec/xi-imgcodec.dll")
if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB)
    target_compile_definitions(cap_jpeg_encode_host_test PRIVATE XINSP2_HAS_TURBOJPEG=1)
    target_include_directories(cap_jpeg_encode_host_test PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(cap_jpeg_encode_host_test PRIVATE ${TURBOJPEG_LIB})
endif()
if(WIN32)
    target_link_libraries(cap_jpeg_encode_host_test PRIVATE Ws2_32)
endif()
add_dependencies(cap_jpeg_encode_host_test imgcodec)
add_test(NAME cap_jpeg_encode_host_test COMMAND cap_jpeg_encode_host_test)

# bench_jpeg — JPEG-encoder benchmark, MOVED here from backend/tests. The encoder
# (xi::encode_jpeg) and its turbojpeg dependency live in this plugins tree post-CUT,
# so the bench that measures them lives here too — the backend build no longer knows
# turbojpeg. Manual dev bench (no ctest / no perf-gate); run it for numbers.
# OpenCV-free (turbo -> stb dispatch). Sources are the shared backend headers +
# stb impl; perf_fingerprint.hpp comes from backend/tests on the include path.
add_executable(bench_jpeg ../backend/tests/bench_jpeg.cpp ../backend/src/stb_impl.cpp)
target_include_directories(bench_jpeg PRIVATE ${XI_INCLUDE} ${XI_VENDOR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../backend/tests)
if(XINSP2_HAS_TURBOJPEG AND TURBOJPEG_INCLUDE_DIR AND TURBOJPEG_LIB)
    target_compile_definitions(bench_jpeg PRIVATE XINSP2_HAS_TURBOJPEG=1)
    target_include_directories(bench_jpeg PRIVATE ${TURBOJPEG_INCLUDE_DIR})
    target_link_libraries(bench_jpeg PRIVATE ${TURBOJPEG_LIB})
endif()

# Resource-handle convention (doc 14 appendix) — demo.lut end to end against
# the REAL built DLL through the REAL host funnel: build -> handle entry
# {type,id,gen,$v}, content dedup (built=0, builds pinned), the handle HOP
# (two consumers, zero rebuild), dump byte-determinism, ring-pressure recycle
# and recycle_all -> stale_handle, wrong type -> wrong_type, malformed/out-of-
# range handles -> bad_handle, handle $v gate, $probe/$v convention,
# unregister-on-destroy, pack balance.
add_executable(cap_lut_owner_test cap_lut_owner_test.cpp)
target_include_directories(cap_lut_owner_test PRIVATE
    ${XI_INCLUDE} ${XI_VENDOR} ${XI_VENDOR}/yyjson)
target_compile_definitions(cap_lut_owner_test PRIVATE
    LUT_OWNER_DLL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/lut_owner/xi-lut_owner.dll")
if(WIN32)
    target_link_libraries(cap_lut_owner_test PRIVATE Ws2_32)
endif()
add_dependencies(cap_lut_owner_test lut_owner)
add_test(NAME cap_lut_owner_test COMMAND cap_lut_owner_test)
