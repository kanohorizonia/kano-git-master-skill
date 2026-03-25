#!/usr/bin/env bash
# =============================================================================
# Windows MSVC Coverage - Run Tests with Collection
# =============================================================================
# Uses Microsoft.CodeCoverage.Console to collect coverage data from test run.
# Output: .coverage binary file (can be converted with ninja-msvc-coverage-report.sh)
#
# Usage:
#   bash ninja-msvc-coverage-run.sh
#
# Environment:
#   KOG_COVERAGE_ROOT  - Output directory for coverage files
#                       (default: $KOG_CPP_ROOT/out/coverage)
#   KOG_CODE_COVERAGE_CONSOLE - Path to Microsoft.CodeCoverage.Console.exe
#                       (auto-detected if not set)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_CPP_ROOT}/out/coverage}"

# Detect CodeCoverage.Console
KOG_CODE_COVERAGE_CONSOLE="${KOG_CODE_COVERAGE_CONSOLE:-}"
if [[ -z "$KOG_CODE_COVERAGE_CONSOLE" ]]; then
    # Search common VS installation paths
    for path in \
        "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe" \
        "C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe"
    do
        if [[ -x "$path" ]]; then
            KOG_CODE_COVERAGE_CONSOLE="$path"
            break
        fi
    done
fi

if [[ -z "$KOG_CODE_COVERAGE_CONSOLE" || ! -x "$KOG_CODE_COVERAGE_CONSOLE" ]]; then
    echo "[ERROR] Microsoft.CodeCoverage.Console not found." >&2
    echo "[ERROR] Set KOG_CODE_COVERAGE_CONSOLE to the path." >&2
    exit 1
fi

# Binary path
BINARY_DIR="$KOG_CPP_ROOT/out/bin/windows-ninja-msvc-coverage/debug"
TEST_BINARY="$BINARY_DIR/kano_git_tui_tests.exe"

if [[ ! -f "$TEST_BINARY" ]]; then
    echo "[ERROR] Test binary not found: $TEST_BINARY" >&2
    echo "[ERROR] Run ninja-msvc-coverage-build.sh first." >&2
    exit 1
fi

mkdir -p "$KOG_COVERAGE_ROOT"
COVERAGE_OUTPUT="$KOG_COVERAGE_ROOT/windows.coverage"

echo "[coverage-run] Binary: $TEST_BINARY"
echo "[coverage-run] Output: $COVERAGE_OUTPUT"
echo "[coverage-run] Tool: $KOG_CODE_COVERAGE_CONSOLE"

"$KOG_CODE_COVERAGE_CONSOLE" collect "$TEST_BINARY" \
    -o "$COVERAGE_OUTPUT" \
    -f coverage \
    2>&1 || {
    echo "[ERROR] Coverage collection failed" >&2
    exit 1
}

echo "[coverage-run] Done. Coverage file: $COVERAGE_OUTPUT"
echo "[coverage-run] Run ninja-msvc-coverage-report.sh to generate HTML report."
