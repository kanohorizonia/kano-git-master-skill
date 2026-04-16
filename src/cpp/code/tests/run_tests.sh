#!/usr/bin/env bash
# Quick script to build and run kano-git C++ tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

resolve_workspace_root() {
  local cursor
  cursor="$(cd "$1" && pwd)"
  while true; do
    if [[ -f "$cursor/scripts/kog" ]]; then
      printf '%s\n' "$cursor"
      return 0
    fi
    local parent
    parent="$(dirname "$cursor")"
    if [[ -z "$parent" || "$parent" == "$cursor" ]]; then
      return 1
    fi
    cursor="$parent"
  done
}

resolve_bin_dir() {
  local cpp_dir="$1"
  local preset_name="$2"
  if [[ -d "$cpp_dir/out/bin/$preset_name" ]]; then
    printf '%s\n' "$cpp_dir/out/bin/$preset_name"
    return 0
  fi
  local canonical
  canonical="$(printf '%s' "$preset_name" | sed -E 's/-(debug|release|relwithdebinfo|minsizerel)$//')"
  if [[ -d "$cpp_dir/out/bin/$canonical" ]]; then
    printf '%s\n' "$cpp_dir/out/bin/$canonical"
    return 0
  fi
  return 1
}

# Default preset (can be overridden)
PRESET="${1:-linux-ninja-gcc-release}"
WITH_E2E="${2:-0}"

echo "Building kano-git tests with preset: $PRESET"
cd "$CPP_ROOT"

WORKSPACE_ROOT="$(resolve_workspace_root "$CPP_ROOT")"
if [[ -z "$WORKSPACE_ROOT" ]]; then
  echo "Cannot resolve workspace root containing kog launcher" >&2
  exit 1
fi

echo "Building via kog self build..."
"$WORKSPACE_ROOT/scripts/kog" self build

BIN_DIR="$(resolve_bin_dir "$CPP_ROOT" "$PRESET")"
EXE_DIR="$BIN_DIR/release"

# Run tests
echo ""
echo "Running CLI tests..."
"$EXE_DIR/kano_git_cli_tests"

echo ""
echo "Running TUI tests..."
"$EXE_DIR/kano_git_tui_tests"

echo ""
echo "Running shell executor focused TUI tests..."
"$EXE_DIR/kano_git_tui_tests" "[shell-executor]"

echo ""
echo "Running commit plan tests..."
"$EXE_DIR/kano_git_commit_plan_tests"

echo ""
echo "All tests completed successfully!"

if [[ "$WITH_E2E" == "1" || "$WITH_E2E" == "--with-e2e" ]]; then
    echo ""
    echo "Running E2E regression tests..."
    "$SCRIPT_DIR/e2e/plan_commit_regression/run.sh" "$WORKSPACE_ROOT"
fi
