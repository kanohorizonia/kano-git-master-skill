#!/usr/bin/env bash
# =============================================================================
# Linux Coverage - Run Tests with Collection via Docker
# =============================================================================
# Runs coverage-instrumented tests inside Docker container.
# Uses docker cp pattern to copy coverage data (.profraw) back to host.
#
# Usage:
#   bash ninja-clang-coverage-run.sh
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

echo "[coverage-run-linux] Starting Docker-based coverage test run..."

# Container name
container_name="kano-git-coverage-$$"

# Convert paths for Docker on Windows
DOCKER_CPP_ROOT="$(to_docker_path "$KOG_CPP_ROOT")"
DOCKER_COVERAGE_ROOT="$(to_docker_path "$KOG_COVERAGE_ROOT")"

# Check if container already exists from build step
if docker ps -a --format '{{.Names}}' | grep -q "^${container_name}$"; then
    echo "[coverage-run-linux] Found existing container from build step, using it..."
    docker start "$container_name" >/dev/null 2>&1
else
    # Start fresh container with source mounted
    echo "[coverage-run-linux] Starting Docker container..."
    docker run -d \
        --name "$container_name" \
        -v "$DOCKER_CPP_ROOT:/workspace/src/cpp" \
        -w /workspace/src/cpp \
        archlinux:latest sleep infinity \
        2>&1 || {
        echo "[ERROR] Failed to start Docker container" >&2
        exit 1
    }

    # Install tools
    echo "[coverage-run-linux] Installing tools..."
    docker exec "$container_name" bash -c "
        pacman -Sy --noconfirm clang llvm git > /dev/null 2>&1
    "
fi

# Cleanup on exit
cleanup() {
    docker rm -f "$container_name" 2>/dev/null || true
}
trap cleanup EXIT

# Binary path inside container (matches build output)
binary_path="/workspace/src/cpp/out/bin/linux-ninja-clang-coverage/debug/kano_git_tui_tests"

if ! docker exec "$container_name" test -f "$binary_path"; then
    echo "[ERROR] Test binary not found inside container: $binary_path" >&2
    echo "[ERROR] Run ninja-clang-coverage-build.sh first." >&2
    exit 1
fi

# Local profraw directory
profraw_dir="$KOG_COVERAGE_ROOT/profraw"
mkdir -p "$profraw_dir"
rm -f "$profraw_dir"/*.profraw 2>/dev/null || true

# Container-side profraw mount point
container_profraw="/workspace/profraw"
docker exec "$container_name" bash -c "mkdir -p $container_profraw"

echo "[coverage-run-linux] Binary: $binary_path"
echo "[coverage-run-linux] Profraw output: $container_profraw"

# Run tests with coverage collection inside container
# Using fixed path (not %m) for simplicity inside container
docker exec "$container_name" bash -c "
    export LLVM_PROFILE_FILE='$container_profraw/run.profraw'
    cd /workspace/src/cpp
    /workspace/src/cpp/out/bin/linux-ninja-clang-coverage/debug/kano_git_tui_tests
" 2>&1

echo "[coverage-run-linux] Copying coverage data from container..."

# Copy profraw file back to host
docker cp "$container_name:$container_profraw/run.profraw" "$profraw_dir/linux.profraw" 2>&1

echo "[coverage-run-linux] Done."
echo "[coverage-run-linux] Coverage file: $profraw_dir/linux.profraw"
echo "[coverage-run-linux] Run ninja-clang-coverage-report.sh to generate HTML report."