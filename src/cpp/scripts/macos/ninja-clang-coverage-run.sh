#!/usr/bin/env bash
# =============================================================================
# macOS Coverage - Run Tests with Collection
# =============================================================================
# Run coverage-instrumented tests on macOS (native or via macBuilder).
#
# Usage:
#   bash ninja-clang-coverage-run.sh
#
# Environment:
#   KOG_COVERAGE_ROOT  - Output directory for coverage files
#                       (default: $KOG_CPP_ROOT/out/coverage)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_CPP_ROOT}/out/coverage}"

echo "[coverage-run-macos] Starting..."

# Binary path
binary_path="$KOG_CPP_ROOT/out/bin/macos-ninja-clang-coverage/debug/kano_git_tui_tests"

# Check if binary exists (native build case)
if [[ ! -f "$binary_path" ]]; then
    echo "[ERROR] Test binary not found: $binary_path" >&2
    echo "[ERROR] Run ninja-clang-coverage-build.sh first." >&2
    exit 1
fi

# Profraw directory
profraw_dir="$KOG_COVERAGE_ROOT/profraw"
mkdir -p "$profraw_dir"
rm -f "$profraw_dir"/*.profraw 2>/dev/null || true

export LLVM_PROFILE_FILE="$profraw_dir/macos.profraw"

echo "[coverage-run-macos] Binary: $binary_path"
echo "[coverage-run-macos] Profile output: $LLVM_PROFILE_FILE"

# Run tests
"$binary_path"

echo "[coverage-run-macos] Done."
echo "[coverage-run-macos] Coverage file: $LLVM_PROFILE_FILE"
echo "[coverage-run-macos] Run ninja-clang-coverage-report.sh to generate HTML report."