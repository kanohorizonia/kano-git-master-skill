#!/usr/bin/env bash
# =============================================================================
# macOS ARM64 Coverage Build - Host Auto-Detection
# =============================================================================
# Build with Clang coverage instrumentation for ARM64.
# On macOS ARM64: native build.
# On non-macOS: remote build via macBuilder.
#
# Usage:
#   bash ninja-clang-arm64-coverage-build.sh
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/../common/unix_preset_build.sh"
source "$SCRIPT_DIR/../common/macos_remote_build.sh"

detect_host_and_build() {
    local host_os
    host_os="$(uname -s 2>/dev/null || true)"

    case "$host_os" in
        Darwin)
            local arch
            arch="$(uname -m 2>/dev/null || true)"
            if [[ "$arch" == "arm64" || "$arch" == "aarch64" ]]; then
                echo "[coverage-build-macos-arm64] macOS ARM64 host detected → native build"
                (
                    cd "$KOG_CPP_ROOT"
                    cmake --preset macos-ninja-clang-arm64-coverage
                    cmake --build --preset macos-ninja-clang-arm64-coverage-debug
                )
            else
                echo "[coverage-build-macos-arm64] macOS x64 host but ARM64 target → remote build via macBuilder"
                kog_remote_build_macos "macos-ninja-clang-arm64-coverage" "Debug"
            fi
            ;;
        *)
            echo "[coverage-build-macos-arm64] Non-macOS host → remote build via macBuilder"
            kog_remote_build_macos "macos-ninja-clang-arm64-coverage" "Debug"
            ;;
    esac
}

detect_host_and_build
