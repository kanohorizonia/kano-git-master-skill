#!/usr/bin/env bash
# =============================================================================
# Linux Coverage - Generate HTML Report via Docker
# =============================================================================
# Uses llvm-cov inside Docker to generate HTML coverage report.
# Expects .profraw file from ninja-clang-coverage-run.sh.
#
# Usage:
#   bash ninja-clang-coverage-report.sh
#
# Environment:
#   KOG_COVERAGE_ROOT  - Local output directory (default: $KOG_CPP_ROOT/out/coverage)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_CPP_ROOT}/out/coverage}"

# Convert Windows path to Docker-compatible format
to_docker_path() {
    local p="$1"
    if [[ "$p" =~ ^[A-Za-z]: ]]; then
        echo "/${p:0:1:1}${p:2}" | tr '\\' '/'
    else
        echo "$p"
    fi
}

# Prevent Git Bash from converting paths
export MSYS_NO_PATHCONV=1

echo "[coverage-report-linux] Starting Docker-based coverage report..."

# Container name
container_name="kano-git-coverage-$$"

# Convert paths for Docker on Windows
DOCKER_CPP_ROOT="$(to_docker_path "$KOG_CPP_ROOT")"
DOCKER_COVERAGE_ROOT="$(to_docker_path "$KOG_COVERAGE_ROOT")"

# Check if container already exists
if docker ps -a --format '{{.Names}}' | grep -q "^${container_name}$"; then
    echo "[coverage-report-linux] Found existing container, using it..."
    docker start "$container_name" >/dev/null 2>&1
else
    # Start container with source and coverage data mounted
    echo "[coverage-report-linux] Starting Docker container..."
    docker run -d \
        --name "$container_name" \
        -v "$DOCKER_CPP_ROOT:/workspace/src/cpp" \
        -v "$DOCKER_COVERAGE_ROOT:/workspace/coverage:rw" \
        -w /workspace/src/cpp \
        ubuntu:24.04 sleep infinity \
        2>&1 || {
        echo "[ERROR] Failed to start Docker container" >&2
        exit 1
    }

    # Install tools
    echo "[coverage-report-linux] Installing tools..."
    docker exec "$container_name" bash -c "
        apt-get update -qq
        apt-get install -y -qq clang llvm llvm-tools > /dev/null 2>&1
    "
fi

# Cleanup on exit
cleanup() {
    docker rm -f "$container_name" 2>/dev/null || true
}
trap cleanup EXIT

# Paths
profraw_file="/workspace/coverage/profraw/linux.profraw"
profdata_file="/workspace/coverage/merged.profdata"
binary_path="/workspace/src/cpp/out/bin/linux-ninja-clang-coverage/debug/kano_git_tui_tests"
html_dir="/workspace/coverage/html"

if [[ ! -f "$profraw_file" ]]; then
    echo "[ERROR] Profraw file not found: $profraw_file" >&2
    echo "[ERROR] Run ninja-clang-coverage-run.sh first." >&2
    exit 1
fi

if [[ ! -f "$binary_path" ]]; then
    echo "[ERROR] Binary not found: $binary_path" >&2
    echo "[ERROR] Run ninja-clang-coverage-build.sh first." >&2
    exit 1
fi

mkdir -p "$html_dir"

# Merge profraw to profdata inside container
echo "[coverage-report-linux] Merging coverage data..."
docker exec "$container_name" bash -c "
    llvm-profdata merge \
        /workspace/coverage/profraw/linux.profraw \
        -o /workspace/coverage/merged.profdata
" 2>&1

if [[ ! -f "$profdata_file" ]]; then
    echo "[ERROR] Failed to create profdata file" >&2
    exit 1
fi

# Generate HTML report inside container
echo "[coverage-report-linux] Generating HTML report..."
docker exec "$container_name" bash -c "
    llvm-cov show \
        /workspace/src/cpp/out/bin/linux-ninja-clang-coverage/debug/kano_git_tui_tests \
        -instr-profile=/workspace/coverage/merged.profdata \
        --format=html \
        --output-dir=/workspace/coverage/html \
        --ignore-filename-regex='_deps|catch2|ftxui|thirdparty|build|\.vcpkg'
" 2>&1 || true

# Copy HTML report back to host if needed (should already be mounted)
if [[ -d "$KOG_COVERAGE_ROOT/html" ]]; then
    echo "[coverage-report-linux] HTML report already available at: $KOG_COVERAGE_ROOT/html"
fi

# Text summary
echo ""
echo "[coverage-report-linux] Text summary:"
docker exec "$container_name" bash -c "
    llvm-cov report \
        /workspace/src/cpp/out/bin/linux-ninja-clang-coverage/debug/kano_git_tui_tests \
        -instr-profile=/workspace/coverage/merged.profdata \
        --ignore-filename-regex='_deps|catch2|ftxui|thirdparty|build|\.vcpkg'
" 2>&1

echo ""
echo "[coverage-report-linux] Reports:"
echo "  HTML:      $KOG_COVERAGE_ROOT/html/index.html"
echo "  Profdata:  $KOG_COVERAGE_ROOT/merged.profdata"