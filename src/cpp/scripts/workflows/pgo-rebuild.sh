#!/usr/bin/env bash
#
# pgo-rebuild.sh — Full PGO pipeline (collect → gather → merge → use)
#
# Keeps the historical parent entrypoint used by profiling/run_matrix.py while
# delegating the actual merge step to shared infra and using preset-aware build
# helpers for host-specific configure/build flows.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
COMMON_ROOT="$CPP_ROOT/scripts/common"
INFRA_COMMON_ROOT="$CPP_ROOT/shared/infra/scripts/common"

PARENT_BUILD_METADATA_SH="$COMMON_ROOT/build_metadata.sh"
PARENT_UNIX_PRESET_BUILD_SH="$COMMON_ROOT/unix_preset_build.sh"
INFRA_WINDOWS_PRESET_BUILD_SH="$INFRA_COMMON_ROOT/windows_preset_build.sh"
PARENT_PGO_GATHER_SH="$CPP_ROOT/scripts/stages/pgo-gather.sh"
INFRA_PGO_WORKFLOW_SH="$INFRA_COMMON_ROOT/pgo_workflow.sh"

require_file() {
  local in_path="$1"
  if [[ ! -f "$in_path" ]]; then
    echo "Required script not found: $in_path" >&2
    exit 1
  fi
}

json_with_pgo_mode() {
  local in_mode="$1"
  python - "$in_mode" <<'PY'
import json
import os
import sys

mode = sys.argv[1]
raw = os.environ.get("KOG_CMAKE_CACHE_ARGS_JSON", "").strip()
data = {}
if raw:
    data = json.loads(raw)
data["KOG_PGO_MODE"] = mode
print(json.dumps(data))
PY
}

is_windows_host() {
  case "$(uname -s 2>/dev/null || true)" in
    MINGW*|MSYS*|CYGWIN*) return 0 ;;
    *) return 1 ;;
  esac
}

is_macos_host() {
  [[ "$(uname -s 2>/dev/null || true)" == "Darwin" ]]
}

default_collect_configure_preset() {
  if is_windows_host; then
    printf '%s\n' "windows-ninja-msvc"
  elif is_macos_host; then
    if [[ "$(uname -m 2>/dev/null || true)" == "arm64" || "$(uname -m 2>/dev/null || true)" == "aarch64" ]]; then
      printf '%s\n' "macos-ninja-clang-arm64"
    else
      printf '%s\n' "macos-ninja-clang-x64"
    fi
  else
    printf '%s\n' "linux-ninja-gcc"
  fi
}

default_collect_build_preset() {
  if is_windows_host; then
    printf '%s\n' "windows-ninja-msvc-debug"
  elif is_macos_host; then
    if [[ "$(uname -m 2>/dev/null || true)" == "arm64" || "$(uname -m 2>/dev/null || true)" == "aarch64" ]]; then
      printf '%s\n' "macos-ninja-clang-arm64-debug"
    else
      printf '%s\n' "macos-ninja-clang-x64-debug"
    fi
  else
    printf '%s\n' "linux-ninja-gcc-debug"
  fi
}

default_use_configure_preset() {
  default_collect_configure_preset
}

default_use_build_preset() {
  if is_windows_host; then
    printf '%s\n' "windows-ninja-msvc-release"
  elif is_macos_host; then
    if [[ "$(uname -m 2>/dev/null || true)" == "arm64" || "$(uname -m 2>/dev/null || true)" == "aarch64" ]]; then
      printf '%s\n' "macos-ninja-clang-arm64-release"
    else
      printf '%s\n' "macos-ninja-clang-x64-release"
    fi
  else
    printf '%s\n' "linux-ninja-gcc-release"
  fi
}

run_collect_build() {
  local configure_preset="${KOG_PGO_COLLECT_CONFIGURE_PRESET:-$(default_collect_configure_preset)}"
  local build_preset="${KOG_PGO_COLLECT_BUILD_PRESET:-$(default_collect_build_preset)}"
  local original_cache_args="${KOG_CMAKE_CACHE_ARGS_JSON:-}"

  export KOG_CPP_ROOT="$CPP_ROOT"
  export KOG_CMAKE_CACHE_ARGS_JSON="$(json_with_pgo_mode collect)"

  if is_windows_host; then
    # shellcheck disable=SC1090
    source "$PARENT_BUILD_METADATA_SH"
    # shellcheck disable=SC1090
    source "$INFRA_WINDOWS_PRESET_BUILD_SH"
    kog_run_windows_preset "$configure_preset" "$build_preset" "${KOG_VCVARS_ARCH:-x64}"
  else
    # shellcheck disable=SC1090
    source "$PARENT_UNIX_PRESET_BUILD_SH"
    kog_run_unix_preset "$configure_preset" "$build_preset"
  fi

  export KOG_CMAKE_CACHE_ARGS_JSON="$original_cache_args"
}

run_use_build() {
  local configure_preset="${KOG_PGO_USE_CONFIGURE_PRESET:-$(default_use_configure_preset)}"
  local build_preset="${KOG_PGO_USE_BUILD_PRESET:-$(default_use_build_preset)}"
  local original_cache_args="${KOG_CMAKE_CACHE_ARGS_JSON:-}"

  export KOG_CPP_ROOT="$CPP_ROOT"
  export KOG_CMAKE_CACHE_ARGS_JSON="$(json_with_pgo_mode use)"

  if is_windows_host; then
    # shellcheck disable=SC1090
    source "$PARENT_BUILD_METADATA_SH"
    # shellcheck disable=SC1090
    source "$INFRA_WINDOWS_PRESET_BUILD_SH"
    kog_run_windows_preset "$configure_preset" "$build_preset" "${KOG_VCVARS_ARCH:-x64}"
  else
    # shellcheck disable=SC1090
    source "$PARENT_UNIX_PRESET_BUILD_SH"
    kog_run_unix_preset "$configure_preset" "$build_preset"
  fi

  export KOG_CMAKE_CACHE_ARGS_JSON="$original_cache_args"
}

main() {
  require_file "$PARENT_BUILD_METADATA_SH"
  require_file "$PARENT_UNIX_PRESET_BUILD_SH"
  require_file "$INFRA_WINDOWS_PRESET_BUILD_SH"
  require_file "$PARENT_PGO_GATHER_SH"
  require_file "$INFRA_PGO_WORKFLOW_SH"

  run_collect_build
  bash "$PARENT_PGO_GATHER_SH"
  bash "$INFRA_PGO_WORKFLOW_SH" merge
  run_use_build
}

main "$@"
