# perf_gate.cmake — run a benchmark in --gate mode, parse its INTEGER timing
# metrics, compare each against a recorded baseline, and FAIL the test if any
# metric regresses by more than THRESHOLD_PCT percent. Slower-is-worse: every
# GATE metric is a time (ns/us), so a larger measured value is a regression.
#
# These are BEST-CASE micro-throughput regression gates (each metric is a
# best-of/minimum over repeated batches — see the benches). They catch a
# same-machine, same-backend slowdown in a hot inner op. They do NOT measure
# p99 / tail / end-to-end latency, and they are only meaningful WITHIN one
# environment class — hence the fingerprint below.
#
# Invoked from ctest as:
#   cmake -DBENCH_EXE=<exe> -DBASELINE=<file> [-DTHRESHOLD_PCT=25]
#         [-DUPDATE_BASELINE=ON] -P perf_gate.cmake
#
# Metrics are emitted by the bench as integer-valued lines:
#   GATE <metric_name> <integer>
# so the comparison uses pure integer CMake math (no float arithmetic):
#   regression  <=>  measured > baseline * (100 + THRESHOLD_PCT) / 100
#
# ENVIRONMENT FINGERPRINT (schema xi.perf-baseline/1)
# ---------------------------------------------------
# A baseline captured on one machine is not comparable on different hardware /
# a different JPEG backend / a debug build — comparing there produces FALSE
# regressions. So each baseline now carries a fingerprint header, written as
# `#fp <key> <value>` comment lines:
#     #fp schema xi.perf-baseline/1
#     #fp cpu <host processor description>
#     #fp logical_cores <n>
#     #fp os <host system + version>
#     #fp build_type <release|debug>
#     #fp opencv <version|none>
#     #fp jpeg_backend <turbojpeg|opencv|ipp|stb>
#     #fp xinsp_commit <short hash|dev|unknown>
# The bench supplies build_type/opencv/jpeg_backend/xinsp_commit (things only it
# can see) as `FP <key> <value>` stdout lines; this script adds the host facts
# (cpu/logical_cores/os). On compare, if the CLASS fields (jpeg_backend, opencv,
# os, build_type) differ from the baseline — or the baseline predates the schema
# and has no fingerprint — the gate is SKIPPED WITH A REASON, never failed.
#
# UPDATE_BASELINE=ON rewrites the baseline (fingerprint + metrics) from this run
# instead of comparing — how the committed baselines are captured on a machine.

if(NOT DEFINED THRESHOLD_PCT)
    set(THRESHOLD_PCT 25)
endif()
if(NOT BENCH_EXE OR NOT EXISTS "${BENCH_EXE}")
    message(FATAL_ERROR "perf_gate: BENCH_EXE not found: '${BENCH_EXE}'")
endif()
if(NOT BASELINE)
    message(FATAL_ERROR "perf_gate: BASELINE path not provided")
endif()

execute_process(
    COMMAND "${BENCH_EXE}" --gate
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE  _err
    RESULT_VARIABLE _rc
    TIMEOUT 300)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "perf_gate: '${BENCH_EXE} --gate' exited ${_rc}\n${_out}\n${_err}")
endif()

# Pull every "GATE <name> <int>" line out of the bench output.
string(REGEX MATCHALL "GATE[ \t]+[A-Za-z0-9_]+[ \t]+[0-9]+" _measured "${_out}")
if(_measured STREQUAL "")
    message(FATAL_ERROR "perf_gate: no GATE metrics in bench output:\n${_out}")
endif()

# --- collect THIS run's fingerprint ---------------------------------------
# Host facts this script can see. The bench supplies the rest via `FP` lines.
cmake_host_system_information(RESULT _cpu   QUERY PROCESSOR_DESCRIPTION)
cmake_host_system_information(RESULT _cores QUERY NUMBER_OF_LOGICAL_CORES)
if(NOT _cpu)
    set(_cpu "unknown")
endif()
string(STRIP "${_cpu}" _cpu)
string(REGEX REPLACE "[\r\n]+" " " _cpu "${_cpu}")
set(_os "${CMAKE_HOST_SYSTEM_NAME} ${CMAKE_HOST_SYSTEM_VERSION}")
string(STRIP "${_os}" _os)

# FP <key> <value> lines from the bench (value may contain dots/dashes).
set(_fp_schema "")
set(_fp_jpeg_backend "")
set(_fp_opencv "")
set(_fp_build_type "")
set(_fp_xinsp_commit "")
string(REGEX MATCHALL "FP[ \t]+[A-Za-z0-9_]+[ \t]+[^\r\n]+" _fp_lines "${_out}")
foreach(_l IN LISTS _fp_lines)
    if(_l MATCHES "FP[ \t]+([A-Za-z0-9_]+)[ \t]+(.+)")
        string(STRIP "${CMAKE_MATCH_2}" _v)
        set("_fp_${CMAKE_MATCH_1}" "${_v}")
    endif()
endforeach()

# The full fingerprint of THIS run (host facts + bench facts).
set(_run_fp_cpu           "${_cpu}")
set(_run_fp_logical_cores "${_cores}")
set(_run_fp_os            "${_os}")
set(_run_fp_build_type    "${_fp_build_type}")
set(_run_fp_opencv        "${_fp_opencv}")
set(_run_fp_jpeg_backend  "${_fp_jpeg_backend}")
set(_run_fp_xinsp_commit  "${_fp_xinsp_commit}")

# --- capture mode: (re)write the baseline and stop ------------------------
if(UPDATE_BASELINE)
    get_filename_component(_exe_name "${BENCH_EXE}" NAME)
    set(_content "# perf baseline for ${_exe_name}\n# captured by perf_gate.cmake UPDATE_BASELINE=ON — slower-is-worse integer metrics\n")
    set(_content "${_content}#fp schema ${_fp_schema}\n")
    set(_content "${_content}#fp cpu ${_run_fp_cpu}\n")
    set(_content "${_content}#fp logical_cores ${_run_fp_logical_cores}\n")
    set(_content "${_content}#fp os ${_run_fp_os}\n")
    set(_content "${_content}#fp build_type ${_run_fp_build_type}\n")
    set(_content "${_content}#fp opencv ${_run_fp_opencv}\n")
    set(_content "${_content}#fp jpeg_backend ${_run_fp_jpeg_backend}\n")
    set(_content "${_content}#fp xinsp_commit ${_run_fp_xinsp_commit}\n")
    foreach(_m IN LISTS _measured)
        string(REGEX MATCH "GATE[ \t]+([A-Za-z0-9_]+)[ \t]+([0-9]+)" _ "${_m}")
        set(_content "${_content}${CMAKE_MATCH_1} ${CMAKE_MATCH_2}\n")
    endforeach()
    file(WRITE "${BASELINE}" "${_content}")
    message(STATUS "perf_gate: wrote baseline ${BASELINE}")
    return()
endif()

if(NOT EXISTS "${BASELINE}")
    message(FATAL_ERROR
        "perf_gate: baseline missing: ${BASELINE}\n"
        "  capture one on this machine with:\n"
        "  cmake -DBENCH_EXE=${BENCH_EXE} -DBASELINE=${BASELINE} -DUPDATE_BASELINE=ON -P perf_gate.cmake")
endif()

# Load baseline: `#fp <key> <value>` header lines into BFP_<key>, and
# `<name> <int>` metric lines into BASE_<name>.
file(STRINGS "${BASELINE}" _base_lines)
foreach(_line IN LISTS _base_lines)
    if(_line MATCHES "^[ \t]*#fp[ \t]+([A-Za-z0-9_]+)[ \t]+(.+)")
        string(STRIP "${CMAKE_MATCH_2}" _bv)
        set("BFP_${CMAKE_MATCH_1}" "${_bv}")
        continue()
    endif()
    if(_line MATCHES "^[ \t]*#" OR _line STREQUAL "")
        continue()
    endif()
    if(_line MATCHES "^[ \t]*([A-Za-z0-9_]+)[ \t]+([0-9]+)")
        set("BASE_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endif()
endforeach()

# --- fingerprint gate: skip-with-reason on a mismatched environment -------
# An old baseline that predates the schema has no fingerprint — do NOT crash,
# skip so it can be re-captured. (ctest treats the skip regex as "not a fail".)
if(NOT DEFINED BFP_schema OR NOT BFP_schema STREQUAL "xi.perf-baseline/1")
    message(STATUS
        "perf_gate: SKIP (no xi.perf-baseline/1 fingerprint in ${BASELINE} — "
        "recapture with UPDATE_BASELINE=ON on this machine)")
    return()
endif()

# CLASS fields whose difference makes the numbers incomparable. cpu / cores /
# commit are recorded for provenance but not gated on (same class, different
# box = still a useful, if noisier, signal; a wrong backend/os/build is not).
set(_mismatch "")
foreach(_k jpeg_backend opencv os build_type)
    set(_want "${BFP_${_k}}")
    set(_have "${_run_fp_${_k}}")
    if(NOT "${_want}" STREQUAL "${_have}")
        set(_mismatch "${_mismatch}    ${_k}: baseline='${_want}'  this-run='${_have}'\n")
    endif()
endforeach()
if(NOT _mismatch STREQUAL "")
    message(STATUS
        "perf_gate: SKIP (environment does not match baseline fingerprint — "
        "not comparable, so not a regression):\n${_mismatch}"
        "  baseline cpu='${BFP_cpu}' cores=${BFP_logical_cores} commit=${BFP_xinsp_commit}\n"
        "  this-run cpu='${_run_fp_cpu}' cores=${_run_fp_logical_cores} commit=${_run_fp_xinsp_commit}\n"
        "  recapture on this machine with UPDATE_BASELINE=ON if this is the new reference.")
    return()
endif()

set(_regressions "")
set(_report "")
foreach(_m IN LISTS _measured)
    string(REGEX MATCH "GATE[ \t]+([A-Za-z0-9_]+)[ \t]+([0-9]+)" _ "${_m}")
    set(_name "${CMAKE_MATCH_1}")
    set(_val  "${CMAKE_MATCH_2}")
    if(NOT DEFINED BASE_${_name})
        set(_report "${_report}  ${_name}: measured=${_val}  (no baseline — skipped)\n")
        continue()
    endif()
    set(_base "${BASE_${_name}}")
    math(EXPR _limit "${_base} * (100 + ${THRESHOLD_PCT}) / 100")
    # pct delta vs baseline, integer, for the report line (signed)
    if(_base GREATER 0)
        math(EXPR _delta "(${_val} - ${_base}) * 100 / ${_base}")
    else()
        set(_delta "?")
    endif()
    if(_val GREATER _limit)
        set(_report "${_report}  ${_name}: measured=${_val}  baseline=${_base}  (+${_delta}%)  > limit ${_limit}  REGRESSION\n")
        list(APPEND _regressions "${_name}")
    else()
        set(_report "${_report}  ${_name}: measured=${_val}  baseline=${_base}  (${_delta}%)  ok (limit ${_limit})\n")
    endif()
endforeach()

message(STATUS "perf_gate ${BENCH_EXE} (threshold +${THRESHOLD_PCT}%, backend=${_run_fp_jpeg_backend}):\n${_report}")

if(NOT _regressions STREQUAL "")
    list(JOIN _regressions ", " _rj)
    message(FATAL_ERROR "perf_gate: PERFORMANCE REGRESSION in: ${_rj} (see report above)")
endif()
