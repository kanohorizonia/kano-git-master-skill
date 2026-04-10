#!/usr/bin/env bash

set -euo pipefail

KOG_PROFILE_COMMON_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
KOG_PROFILE_SCRIPT_ROOT="$(cd -- "$KOG_PROFILE_COMMON_DIR/.." && pwd)"
KOG_PROFILE_CPP_ROOT="$(cd -- "$KOG_PROFILE_SCRIPT_ROOT/.." && pwd)"
KOG_PROFILE_REPO_ROOT="$(cd -- "$KOG_PROFILE_CPP_ROOT/../.." && pwd)"
KOG_PROFILE_TMP_ROOT="${KOG_PROFILE_TMP_ROOT:-$KOG_PROFILE_REPO_ROOT/.kano/tmp/profiling}"
KOG_PROFILE_REPORT_ROOT="${KOG_PROFILE_REPORT_ROOT:-$KOG_PROFILE_REPO_ROOT/docs/profiling}"

kog_profile_host_os() {
  local os_name
  os_name="$(uname -s 2>/dev/null || true)"
  case "$os_name" in
    MINGW*|MSYS*|CYGWIN*) printf '%s\n' windows ;;
    Darwin) printf '%s\n' macos ;;
    *) printf '%s\n' linux ;;
  esac
}

kog_profile_arch() {
  local arch
  arch="$(uname -m 2>/dev/null || true)"
  case "$arch" in
    aarch64|arm64) printf '%s\n' arm64 ;;
    *) printf '%s\n' x64 ;;
  esac
}

kog_profile_resolve_matrix() {
  local matrix_name="${1:-default}"
  printf '%s\n' "$KOG_PROFILE_COMMON_DIR/matrices/${matrix_name}.json"
}

kog_profile_require_matrix() {
  local matrix_path
  matrix_path="$(kog_profile_resolve_matrix "$1")"
  [[ -f "$matrix_path" ]] || {
    echo "profiling matrix not found: $matrix_path" >&2
    return 1
  }
  printf '%s\n' "$matrix_path"
}
