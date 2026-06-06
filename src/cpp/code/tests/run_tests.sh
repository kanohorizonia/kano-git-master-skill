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
  local canonical archless candidate
  canonical="$(printf '%s' "$preset_name" | sed -E 's/-(debug|release|relwithdebinfo|minsizerel)$//')"
  archless="$(printf '%s' "$canonical" | sed -E 's/-(x64|arm64)$//')"
  for candidate in "$preset_name" "$canonical" "$archless"; do
    [[ -n "$candidate" ]] || continue
    if [[ -d "$cpp_dir/out/bin/$candidate" ]]; then
      printf '%s\n' "$cpp_dir/out/bin/$candidate"
      return 0
    fi
  done
  return 1
}

resolve_test_exe_dir() {
  local bin_dir="$1"
  local config_dir=""
  for config_dir in release relwithdebinfo minsizerel debug; do
    if [[ ( -x "$bin_dir/$config_dir/kano_git_cli_tests" || -x "$bin_dir/$config_dir/kano_git_cli_tests.exe" ) &&
          ( -x "$bin_dir/$config_dir/kano_git_tui_tests" || -x "$bin_dir/$config_dir/kano_git_tui_tests.exe" ) ]]; then
      printf '%s\n' "$bin_dir/$config_dir"
      return 0
    fi
  done
  return 1
}

has_test_binaries_for_preset() {
  local cpp_dir="$1"
  local preset_name="$2"
  local bin_dir=""
  if ! bin_dir="$(resolve_bin_dir "$cpp_dir" "$preset_name")"; then
    return 1
  fi
  resolve_test_exe_dir "$bin_dir" >/dev/null
}

# Default preset (can be overridden)
PRESET="${1:-linux-ninja-gcc-release}"
LANE_MODE="${2:-default}"
WITH_E2E="${3:-0}"
TEST_XML_OUTPUT="${KANO_TEST_XML:-}"
TEST_XML_DIR=""

run_test_binary() {
  local binary_name="$1"
  shift
  KANO_TEST_BINARY_NAME="$binary_name" "$@"
}

resolve_python() {
  if command -v python3 >/dev/null 2>&1; then
    command -v python3
    return 0
  fi
  if command -v python >/dev/null 2>&1; then
    command -v python
    return 0
  fi
  echo "ERROR: python3 or python is required to merge JUnit XML." >&2
  return 1
}

if [[ -n "${KANO_REPORT_ROOT:-}" ]]; then
  export KANO_BDD_METADATA_DIR="${KANO_BDD_METADATA_DIR:-$KANO_REPORT_ROOT/raw/bdd-metadata}"
  rm -rf -- "$KANO_BDD_METADATA_DIR"
  mkdir -p "$KANO_BDD_METADATA_DIR"
fi

if [[ -n "$TEST_XML_OUTPUT" ]]; then
  TEST_XML_DIR="$(dirname "$TEST_XML_OUTPUT")/.tmp-test-result"
  rm -rf -- "$TEST_XML_DIR"
  mkdir -p "$TEST_XML_DIR"
fi

if [[ "$LANE_MODE" == "--with-e2e" ]]; then
  LANE_MODE="full"
  WITH_E2E="--with-e2e"
fi

echo "Building kano-git tests with preset: $PRESET"
cd "$CPP_ROOT"

WORKSPACE_ROOT="$(resolve_workspace_root "$CPP_ROOT")"
if [[ -z "$WORKSPACE_ROOT" ]]; then
  echo "Cannot resolve workspace root containing kog launcher" >&2
  exit 1
fi

if [[ "${KANO_SKIP_TEST_BUILD:-0}" == "1" || "${KANO_SKIP_TEST_BUILD:-}" == "true" ]]; then
  echo "Skipping build before tests because KANO_SKIP_TEST_BUILD is enabled."
elif has_test_binaries_for_preset "$CPP_ROOT" "$PRESET"; then
  echo "Skipping build before tests because preset '$PRESET' binaries already exist."
else
  echo "Building via kog self build..."
  "$WORKSPACE_ROOT/scripts/kog" self build
fi

BIN_DIR="$(resolve_bin_dir "$CPP_ROOT" "$PRESET" || true)"
if [[ -z "$BIN_DIR" ]]; then
  echo "Could not resolve build output directory for preset '$PRESET'." >&2
  exit 1
fi
EXE_DIR="$(resolve_test_exe_dir "$BIN_DIR" || true)"
if [[ -z "$EXE_DIR" ]]; then
  echo "Could not locate CLI/TUI test binaries under '$BIN_DIR' for preset '$PRESET'." >&2
  exit 1
fi
echo "Using test binaries from: $EXE_DIR"

# Run tests
run_cli_tests() {
  local -a args=()
  case "$LANE_MODE" in
    quick)
      args=("[help],[unknown-command],[log],[output],[infrastructure]")
      ;;
    default|test)
      args=("[help],[unknown-command],[log],[output],[infrastructure]")
      ;;
    full)
      args=("[help],[unknown-command],[log],[output],[infrastructure],[submodule]")
      ;;
    *)
      args=()
      ;;
  esac
  if [[ -n "$TEST_XML_DIR" ]]; then
    run_test_binary "kano_git_cli_tests" "$EXE_DIR/kano_git_cli_tests" "${args[@]}" --reporter junit --out "$TEST_XML_DIR/kano_git_cli_tests.xml"
  else
    run_test_binary "kano_git_cli_tests" "$EXE_DIR/kano_git_cli_tests" "${args[@]}"
  fi
}

run_tui_tests() {
  local -a args=()
  case "$LANE_MODE" in
    quick)
      args=("[unit],[metadata_cache],[infrastructure]")
      ;;
    default|test)
      args=("~[Property]")
      ;;
    full)
      args=()
      ;;
    *)
      args=()
      ;;
  esac
  if [[ -n "$TEST_XML_DIR" ]]; then
    run_test_binary "kano_git_tui_tests" "$EXE_DIR/kano_git_tui_tests" "${args[@]}" --reporter junit --out "$TEST_XML_DIR/kano_git_tui_tests.xml"
  else
    run_test_binary "kano_git_tui_tests" "$EXE_DIR/kano_git_tui_tests" "${args[@]}"
  fi
}

echo ""
echo "Running CLI tests..."
run_cli_tests

echo ""
echo "Running TUI tests..."
run_tui_tests

if [[ "$LANE_MODE" == "full" && "${KANO_FULL_LANE_EXTRA:-0}" == "1" ]]; then
  echo ""
  echo "Running shell executor focused TUI tests..."
  if [[ -n "$TEST_XML_DIR" ]]; then
    run_test_binary "kano_git_tui_tests" "$EXE_DIR/kano_git_tui_tests" "[shell-executor]" --reporter junit --out "$TEST_XML_DIR/kano_git_tui_tests_shell_executor.xml"
  else
    run_test_binary "kano_git_tui_tests" "$EXE_DIR/kano_git_tui_tests" "[shell-executor]"
  fi
fi

if [[ "$LANE_MODE" == "full" && "${KANO_FULL_LANE_EXTRA:-0}" == "1" ]]; then
  echo ""
  echo "Running commit plan tests..."
  if [[ -n "$TEST_XML_DIR" ]]; then
    run_test_binary "kano_git_commit_plan_tests" "$EXE_DIR/kano_git_commit_plan_tests" --reporter junit --out "$TEST_XML_DIR/kano_git_commit_plan_tests.xml"
  else
    run_test_binary "kano_git_commit_plan_tests" "$EXE_DIR/kano_git_commit_plan_tests"
  fi
fi

echo ""
echo "All tests completed successfully!"

if [[ "$WITH_E2E" == "1" || "$WITH_E2E" == "--with-e2e" ]]; then
    echo ""
    echo "Running E2E regression tests..."
    "$SCRIPT_DIR/e2e/plan_commit_regression/run.sh" "$WORKSPACE_ROOT"
fi

if [[ -n "$TEST_XML_OUTPUT" ]]; then
  PYTHON_BIN="$(resolve_python)"
  "$PYTHON_BIN" - "$TEST_XML_DIR" "$TEST_XML_OUTPUT" <<'PY'
from __future__ import annotations
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

in_dir = Path(sys.argv[1])
out_path = Path(sys.argv[2])
root = ET.Element("testsuites")
for xml_path in sorted(in_dir.glob("*.xml")):
    try:
        doc = ET.parse(xml_path).getroot()
    except ET.ParseError:
        continue
    if doc.tag == "testsuite":
        root.append(doc)
    elif doc.tag == "testsuites":
        for suite in doc.findall("testsuite"):
            root.append(suite)

out_path.parent.mkdir(parents=True, exist_ok=True)
ET.ElementTree(root).write(out_path, encoding="utf-8", xml_declaration=True)
PY
fi
