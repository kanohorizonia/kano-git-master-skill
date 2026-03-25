#!/usr/bin/env bash
# =============================================================================
# macOS Coverage - Generate HTML Report
# =============================================================================
# Uses llvm-cov (Xcode) to generate HTML coverage report.
#
# Usage:
#   bash ninja-clang-coverage-report.sh
#
# Environment:
#   KOG_COVERAGE_ROOT  - Output directory for coverage files
#                       (default: $KOG_CPP_ROOT/out/coverage)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_CPP_ROOT}/out/coverage}"

source "$SCRIPT_DIR/../common/private_repo_path.sh"

echo "[coverage-report-macos] Starting..."

# Binary path
binary_path="$KOG_CPP_ROOT/out/bin/macos-ninja-clang-coverage/debug/kano_git_tui_tests"

if [[ ! -f "$binary_path" ]]; then
    echo "[ERROR] Binary not found: $binary_path" >&2
    exit 1
fi

# Profraw/Profdata paths
profraw_dir="$KOG_COVERAGE_ROOT/profraw"
profdata_file="$KOG_COVERAGE_ROOT/merged.profdata"
html_dir="$KOG_COVERAGE_ROOT/html"

mkdir -p "$profraw_dir"
mkdir -p "$html_dir"

# Find llvm-cov (Xcode path)
llvm_cov=""
if [[ -x "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov" ]]; then
    llvm_cov="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-cov"
elif command -v llvm-cov >/dev/null 2>&1; then
    llvm_cov="llvm-cov"
else
    echo "[ERROR] llvm-cov not found. Install Xcode or LLVM tools." >&2
    exit 1
fi

# Find llvm-profdata
llvm_profdata=""
if [[ -x "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-profdata" ]]; then
    llvm_profdata="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/llvm-profdata"
elif command -v llvm-profdata >/dev/null 2>&1; then
    llvm_profdata="llvm-profdata"
fi

if [[ -z "$llvm_profdata" ]]; then
    echo "[ERROR] llvm-profdata not found." >&2
    exit 1
fi

# Merge profraw files
profraw_file="$profraw_dir/macos.profraw"
if [[ ! -f "$profraw_file" ]]; then
    echo "[ERROR] Profraw file not found: $profraw_file" >&2
    echo "[ERROR] Run ninja-clang-coverage-run.sh first." >&2
    exit 1
fi

echo "[coverage-report-macos] Merging coverage data..."
"$llvm_profdata" merge "$profraw_file" -o "$profdata_file" 2>&1

# Generate HTML report
echo "[coverage-report-macos] Generating HTML report..."
"$llvm_cov" show \
    "$binary_path" \
    -instr-profile="$profdata_file" \
    --format=html \
    --output-dir="$html_dir" \
    --ignore-filename-regex="_deps|catch2|ftxui|thirdparty|build|\.vcpkg" 2>&1 || true

# Text summary
echo ""
echo "[coverage-report-macos] Text summary:"
"$llvm_cov" report \
    "$binary_path" \
    -instr-profile="$profdata_file" \
    --ignore-filename-regex="_deps|catch2|ftxui|thirdparty|build|\.vcpkg" 2>&1

echo ""
echo "[coverage-report-macos] Reports:"
echo "  HTML:      $html_dir/index.html"
echo "  Profdata:  $profdata_file"