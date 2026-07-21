#!/usr/bin/env bash
# One-click setup + health check of xInsp2's C++ toolchain on Linux (Debian/Ubuntu,
# x86_64 or aarch64 — developed on a Raspberry Pi 5 / Debian trixie / g++ 14).
#
# Installs what the backend needs to BUILD and to RUN: the backend JIT-compiles
# inspection scripts and project plugins at runtime with g++, so the compiler and
# OpenCV headers are runtime dependencies, not just build-time ones.
#
#   Required:  g++ (>=13, C++20), cmake >= 3.16, ninja, pkg-config, OpenCV 4.x dev
#   Optional:  ccache (much faster rebuilds), libjpeg-turbo dev (fast JPEG encode),
#              node >= 18 (ui-components / vscode-extension / gate docs+sdk stages),
#              python3 (tools/gate.py, examples/qa_* e2e drivers)
#
# Re-runnable: anything already present is detected and skipped. Finishes with a
# health check, and with --check it ONLY runs the health check (installs nothing).
#
# Usage:
#   tools/setup-linux.sh              # install required + optional, then health check
#   tools/setup-linux.sh --check      # health check only, no install, no sudo
#   tools/setup-linux.sh --no-optional
#
# See docs/roadmap/linux-port.md for what is ported, ctest-green, and deferred.
set -uo pipefail

CHECK_ONLY=0
WITH_OPTIONAL=1
for arg in "$@"; do
  case "$arg" in
    --check)       CHECK_ONLY=1 ;;
    --no-optional) WITH_OPTIONAL=0 ;;
    -h|--help)     sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "unknown flag: $arg (try --help)" >&2; exit 2 ;;
  esac
done

if [[ -t 1 ]]; then C_I=$'\033[36m'; C_OK=$'\033[32m'; C_W=$'\033[33m'; C_E=$'\033[31m'; C_0=$'\033[0m'
else C_I=""; C_OK=""; C_W=""; C_E=""; C_0=""; fi
info(){ echo "${C_I}[setup]${C_0} $*"; }
ok(){   echo "  ${C_OK}OK${C_0}   $*"; }
warn(){ echo "  ${C_W}WARN${C_0} $*"; }
err(){  echo "  ${C_E}FAIL${C_0} $*"; }

FAILED=0

# --- preconditions --------------------------------------------------------
[[ "$(uname -s)" == "Linux" ]] || { err "Linux-only. Windows: tools/setup-windows.ps1"; exit 1; }

SUDO=""
if [[ $CHECK_ONLY -eq 0 && $EUID -ne 0 ]]; then
  command -v sudo >/dev/null || { err "need root or sudo to install packages (or re-run with --check)"; exit 1; }
  SUDO="sudo"
fi

APT=""
command -v apt-get >/dev/null && APT=1

# --- 1. install -----------------------------------------------------------
REQUIRED_PKGS=(build-essential g++ cmake ninja-build pkg-config libopencv-dev)
OPTIONAL_PKGS=(ccache libturbojpeg0-dev nodejs npm python3)

if [[ $CHECK_ONLY -eq 0 ]]; then
  if [[ -z "$APT" ]]; then
    warn "no apt-get — install these with your package manager, then re-run with --check:"
    warn "  required: ${REQUIRED_PKGS[*]}"
    warn "  optional: ${OPTIONAL_PKGS[*]}"
  else
    want=()
    for p in "${REQUIRED_PKGS[@]}"; do
      dpkg -s "$p" >/dev/null 2>&1 || want+=("$p")
    done
    if [[ $WITH_OPTIONAL -eq 1 ]]; then
      for p in "${OPTIONAL_PKGS[@]}"; do
        # libturbojpeg0-dev is named libjpeg62-turbo-dev on some releases; try both
        if ! dpkg -s "$p" >/dev/null 2>&1 && apt-cache show "$p" >/dev/null 2>&1; then want+=("$p"); fi
      done
    fi
    if [[ ${#want[@]} -eq 0 ]]; then
      info "all packages already installed — nothing to do"
    else
      info "installing: ${want[*]}"
      $SUDO apt-get update -qq
      $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y "${want[@]}" || {
        err "apt-get install failed"; FAILED=1; }
    fi
  fi
fi

# --- 2. health check ------------------------------------------------------
# ver_ge A B  -> true if version A >= B
ver_ge(){ [[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" == "$2" ]]; }

check_cmd(){ # label  cmd  min-version-or-""  required(0/1)  version-extractor
  local label="$1" cmd="$2" min="$3" req="$4" extract="${5:-}"
  local path; path="$(command -v "$cmd" 2>/dev/null)"
  if [[ -z "$path" ]]; then
    if [[ "$req" == 1 ]]; then err "$(printf '%-16s MISSING (required)' "$label")"; FAILED=1
    else warn "$(printf '%-16s not installed (optional)' "$label")"; fi
    return
  fi
  local v=""
  [[ -n "$extract" ]] && v="$(eval "$extract" 2>/dev/null)"
  if [[ -n "$min" && -n "$v" ]] && ! ver_ge "$v" "$min"; then
    if [[ "$req" == 1 ]]; then err "$(printf '%-16s %s (need >= %s)' "$label" "$v" "$min")"; FAILED=1
    else warn "$(printf '%-16s %s (want >= %s)' "$label" "$v" "$min")"; fi
    return
  fi
  ok "$(printf '%-16s %s %s' "$label" "$path" "${v:+($v)}")"
}

echo
info "Toolchain health ($(uname -m), $( . /etc/os-release 2>/dev/null && echo "${PRETTY_NAME:-unknown}" )):"

check_cmd "g++"        g++        13     1 "g++ -dumpfullversion"
check_cmd "cmake"      cmake      3.16   1 "cmake --version | head -1 | awk '{print \$3}'"
check_cmd "ninja"      ninja      ""     1 "ninja --version"
check_cmd "pkg-config" pkg-config ""     1 ""

# OpenCV: required, and needed at RUNTIME too (the JIT compile driver resolves it
# through pkg-config — see xi_script_compiler.hpp's compile_posix_build_).
if ocv="$(pkg-config --modversion opencv4 2>/dev/null)"; then
  if ver_ge "$ocv" 4.0; then ok "$(printf '%-16s %s (pkg-config opencv4)' "OpenCV" "$ocv")"
  else err "$(printf '%-16s %s (need 4.x)' "OpenCV" "$ocv")"; FAILED=1; fi
else
  err "$(printf '%-16s MISSING (required) — apt install libopencv-dev' "OpenCV")"; FAILED=1
fi

check_cmd "ccache"     ccache     ""     0 "ccache --version | head -1 | awk '{print \$3}'"
check_cmd "node"       node       18     0 "node --version | tr -d v"
check_cmd "python3"    python3    3.9    0 "python3 -c 'import sys;print(\"%d.%d\"%sys.version_info[:2])'"

# libjpeg-turbo: optional accelerator, consumed by the imgcodec PLUGIN
# (-DXINSP2_HAS_TURBOJPEG=ON), not by the backend exe.
if pkg-config --exists libturbojpeg 2>/dev/null; then
  ok "$(printf '%-16s %s (pkg-config libturbojpeg)' "libjpeg-turbo" "$(pkg-config --modversion libturbojpeg)")"
elif [[ -f /usr/include/turbojpeg.h ]]; then
  ok "$(printf '%-16s %s' "libjpeg-turbo" "/usr/include/turbojpeg.h")"
else
  warn "$(printf '%-16s not installed (optional; imgcodec falls back to stb)' "libjpeg-turbo")"
fi

echo
if [[ $FAILED -ne 0 ]]; then
  err "Toolchain incomplete — fix the FAILs above (re-run without --check to install)."
  exit 1
fi

info "Done. Build the backend with:"
cat <<'EOF'

  cmake -S backend -B backend/build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build backend/build -j"$(nproc)"
  ctest --test-dir backend/build --output-on-failure

  # plugins (mock_camera, blob_analysis, imgcodec, …)
  cmake -S plugins -B plugins/build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build plugins/build -j"$(nproc)"

  # web UI components (Node)
  cd ui-components && npm install && npm run build && npm test
EOF
