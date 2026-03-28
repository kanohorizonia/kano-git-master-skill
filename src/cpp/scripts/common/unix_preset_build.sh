#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${KOG_CPP_ROOT:-}" ]]; then
  echo "KOG_CPP_ROOT is not set." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source infra's generic unix preset runner (provides kano_cpp_run_unix_preset)
# shellcheck disable=SC1091
source "$SCRIPT_DIR/../../../shared/infra/scripts/common/unix_preset_build.sh"

# Source local build_metadata.sh (already sources infra via shared/infra)
source "$SCRIPT_DIR/build_metadata.sh"

# =============================================================================
# kog_run_unix_preset — git-master wrapper around kano_cpp_run_unix_preset
# =============================================================================
# Applies git-master-specific overrides (LLVM prefix, modules) then delegates.
# =============================================================================
kog_run_unix_preset() {
  local in_configure_preset="$1"
  local in_build_preset="$2"
  local -a extra_args=()
  local llvm_prefix=""
  local sdk_path=""
  local arch=""
  local preset_name=""

  if [[ "${KOG_BUILD_ENABLE_MODULES:-0}" == "1" ]]; then
    extra_args+=("-DKOG_ENABLE_MODULES=ON")
  else
    extra_args+=("-DKOG_ENABLE_MODULES=OFF")
  fi

  if [[ "$(uname -s 2>/dev/null || true)" == "Darwin" && "${KOG_BUILD_USE_LLVM:-0}" == "1" ]]; then
    llvm_prefix="$(brew --prefix llvm 2>/dev/null || true)"
    if [[ -z "$llvm_prefix" || ! -x "$llvm_prefix/bin/clang" || ! -x "$llvm_prefix/bin/clang++" ]]; then
      echo "Homebrew LLVM is required for --llvm mode. Install with: brew install llvm" >&2
      return 1
    fi

    sdk_path="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"

    arch="$(uname -m 2>/dev/null || true)"
    if [[ "$arch" == "arm64" || "$arch" == "aarch64" ]]; then
      preset_name="macos-ninja-llvm-arm64"
    else
      preset_name="macos-ninja-llvm-x64"
    fi

    extra_args+=(
      "-DCMAKE_C_COMPILER=$llvm_prefix/bin/clang"
      "-DCMAKE_CXX_COMPILER=$llvm_prefix/bin/clang++"
      "-DKOG_PRESET_NAME=$preset_name"
    )
    if [[ -n "$sdk_path" ]]; then
      extra_args+=("-DCMAKE_OSX_SYSROOT=$sdk_path")
    fi
  fi

  (
    cd "$KOG_CPP_ROOT"
    kog_apply_self_build_config
    kog_collect_build_metadata
    cmake --preset "$in_configure_preset" "${extra_args[@]}"
    cmake --build --preset "$in_build_preset"
  )
}
