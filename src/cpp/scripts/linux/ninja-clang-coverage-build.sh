#!/usr/bin/env bash
# =============================================================================
# Linux Coverage Build via Docker
# =============================================================================
# Builds with coverage instrumentation using Docker.
# Output is copied back to host via docker cp.
#
# Usage:
#   bash ninja-clang-coverage-build.sh
#
# Environment:
#   KOG_COVERAGE_ROOT  - Local output directory (default: $KOG_CPP_ROOT/out/coverage)
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_CPP_ROOT}/out/coverage}"

# Source Docker helper
source "$SCRIPT_DIR/../common/docker_linux_build.sh"

echo "[coverage-build-linux] Starting Docker-based coverage build..."

# Container name
container_name="kano-git-coverage-$$"

docker run -d \
    --name "$container_name" \
    -v "$KOG_CPP_ROOT:/workspace/src/cpp:ro" \
    -w /workspace/src/cpp \
    ubuntu:24.04 sleep infinity \
    2>&1 || {
    echo "[ERROR] Failed to start Docker container" >&2
    exit 1
}

# Cleanup on exit
cleanup() {
    docker rm -f "$container_name" 2>/dev/null || true
}
trap cleanup EXIT

# Install tools and build
echo "[coverage-build-linux] Installing tools..."
docker exec "$container_name" bash -c "
    apt-get update -qq
    apt-get install -y -qq cmake ninja-build clang llvm > /dev/null 2>&1
"

echo "[coverage-build-linux] Configuring coverage build..."
docker exec "$container_name" bash -c "
    cd /workspace/src/cpp
    cmake --preset linux-ninja-clang-coverage
"

echo "[coverage-build-linux] Building..."
docker exec "$container_name" bash -c "
    cd /workspace/src/cpp
    cmake --build --preset linux-ninja-clang-coverage-debug
"

# Copy build output back to host
echo "[coverage-build-linux] Copying build artifacts to host..."
mkdir -p "$KOG_COVERAGE_ROOT/linux-out"
docker cp "$container_name:/workspace/src/cpp/out" "$KOG_COVERAGE_ROOT/linux-out-tmp" 2>&1

# Move to final location
rm -rf "$KOG_COVERAGE_ROOT/linux-out"
mv "$KOG_COVERAGE_ROOT/linux-out-tmp" "$KOG_COVERAGE_ROOT/linux-out"

echo "[coverage-build-linux] Done."
echo "[coverage-build-linux] Build artifacts: $KOG_COVERAGE_ROOT/linux-out"
echo "[coverage-build-linux] Run tests: ninja-clang-coverage-run.sh"
