#!/usr/bin/env bash
# Build kano-git CLI (Unix)
# Usage: ./build.sh [--debug|--release] [--clean]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CONFIG="release"
CLEAN=false

for arg in "$@"; do
    case "$arg" in
        --debug)   CONFIG="debug" ;;
        --release) CONFIG="release" ;;
        --clean)   CLEAN=true ;;
        --help|-h) echo "Usage: $0 [--debug|--release] [--clean]"; exit 0 ;;
    esac
done

BUILD_DIR="build/${CONFIG}"

if $CLEAN && [ -d "$BUILD_DIR" ]; then
    echo "Cleaning ${BUILD_DIR}..."
    rm -rf "$BUILD_DIR"
fi

echo "Configuring (${CONFIG})..."
if [ -n "${VCPKG_ROOT:-}" ]; then
    cmake --preset "${CONFIG}"
else
    # Fallbacks
    TOOLCHAIN=""
    for p in "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake" "$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake"; do
        if [ -f "$p" ]; then
            TOOLCHAIN="$p"
            break
        fi
    done
    if [ -n "$TOOLCHAIN" ]; then
        cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CONFIG^}" -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
    else
        cmake -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${CONFIG^}"
    fi
fi

echo "Building..."
cmake --build "${BUILD_DIR}" --config "$(echo "$CONFIG" | sed 's/./\U&/')"

echo ""
echo "Build complete: ${BUILD_DIR}/kano-git"
echo "Run: ./${BUILD_DIR}/kano-git version"
