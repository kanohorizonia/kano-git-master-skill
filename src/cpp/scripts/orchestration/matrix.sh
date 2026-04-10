#!/usr/bin/env bash

set -euo pipefail

KOG_MATRIX_SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
KOG_MATRIX_CPP_ROOT="$(cd -- "$KOG_MATRIX_SCRIPT_DIR/../.." && pwd)"
KOG_MATRIX_BASE="$KOG_MATRIX_CPP_ROOT/scripts"

kog_matrix_host_os() {
  local os_name
  os_name="$(uname -s 2>/dev/null || true)"
  case "$os_name" in
    MINGW*|MSYS*|CYGWIN*) printf '%s\n' windows ;;
    Darwin) printf '%s\n' macos ;;
    *) printf '%s\n' linux ;;
  esac
}

kog_matrix_arch() {
  local arch
  arch="$(uname -m 2>/dev/null || true)"
  case "$arch" in
    aarch64|arm64) printf '%s\n' arm64 ;;
    *) printf '%s\n' x64 ;;
  esac
}

kog_matrix_default_release_script() {
  local os_name arch
  os_name="$(kog_matrix_host_os)"
  arch="$(kog_matrix_arch)"
  case "$os_name" in
    windows)
      if [[ "$arch" == "arm64" ]]; then
        printf '%s\n' "$KOG_MATRIX_BASE/windows/ninja-msvc-arm64-release.sh"
      else
        printf '%s\n' "$KOG_MATRIX_BASE/windows/ninja-msvc-release.sh"
      fi
      ;;
    macos)
      if [[ "$arch" == "arm64" ]]; then
        printf '%s\n' "$KOG_MATRIX_BASE/macos/ninja-clang-arm64-release.sh"
      else
        printf '%s\n' "$KOG_MATRIX_BASE/macos/ninja-clang-x64-release.sh"
      fi
      ;;
    *)
      printf '%s\n' "$KOG_MATRIX_BASE/linux/ninja-gcc-release.sh"
      ;;
  esac
}

kog_matrix_default_test_report_script() {
  local os_name arch
  os_name="$(kog_matrix_host_os)"
  arch="$(kog_matrix_arch)"
  case "$os_name" in
    windows) printf '%s\n' "$KOG_MATRIX_BASE/windows/test-report.sh" ;;
    macos)
      if [[ "$arch" == "arm64" ]]; then
        printf '%s\n' "$KOG_MATRIX_BASE/macos/test-report-arm64.sh"
      else
        printf '%s\n' "$KOG_MATRIX_BASE/macos/test-report.sh"
      fi
      ;;
    *) printf '%s\n' "$KOG_MATRIX_BASE/linux/test-report.sh" ;;
  esac
}

kog_matrix_default_coverage_build_script() {
  local os_name arch
  os_name="$(kog_matrix_host_os)"
  arch="$(kog_matrix_arch)"
  case "$os_name" in
    windows) printf '%s\n' "$KOG_MATRIX_BASE/windows/ninja-msvc-coverage-build.sh" ;;
    macos)
      if [[ "$arch" == "arm64" ]]; then
        printf '%s\n' "$KOG_MATRIX_BASE/macos/ninja-clang-arm64-coverage-build.sh"
      else
        printf '%s\n' "$KOG_MATRIX_BASE/macos/ninja-clang-coverage-build.sh"
      fi
      ;;
    *) printf '%s\n' "$KOG_MATRIX_BASE/linux/ninja-clang-coverage-build.sh" ;;
  esac
}

kog_matrix_default_coverage_gather_script() {
  local os_name arch
  os_name="$(kog_matrix_host_os)"
  arch="$(kog_matrix_arch)"
  case "$os_name" in
    windows) printf '%s\n' "$KOG_MATRIX_BASE/windows/ninja-msvc-coverage-run.sh" ;;
    macos)
      if [[ "$arch" == "arm64" ]]; then
        printf '%s\n' "$KOG_MATRIX_BASE/macos/ninja-clang-arm64-coverage-run.sh"
      else
        printf '%s\n' "$KOG_MATRIX_BASE/macos/ninja-clang-coverage-run.sh"
      fi
      ;;
    *) printf '%s\n' "$KOG_MATRIX_BASE/linux/ninja-clang-coverage-run.sh" ;;
  esac
}

kog_matrix_default_coverage_report_script() {
  local backend os_name arch
  backend="${1:-default}"
  os_name="$(kog_matrix_host_os)"
  arch="$(kog_matrix_arch)"
  case "$os_name" in
    windows)
      if [[ "$backend" == "opencppcoverage" ]]; then
        printf '%s\n' "$KOG_MATRIX_BASE/windows/coverage-report-opencppcoverage.sh"
      else
        printf '%s\n' "$KOG_MATRIX_BASE/windows/coverage-report-microsoft.sh"
      fi
      ;;
    macos)
      if [[ "$arch" == "arm64" ]]; then
        printf '%s\n' "$KOG_MATRIX_BASE/macos/coverage-report-llvm-arm64.sh"
      else
        printf '%s\n' "$KOG_MATRIX_BASE/macos/coverage-report-llvm.sh"
      fi
      ;;
    *) printf '%s\n' "$KOG_MATRIX_BASE/linux/coverage-report-llvm.sh" ;;
  esac
}
