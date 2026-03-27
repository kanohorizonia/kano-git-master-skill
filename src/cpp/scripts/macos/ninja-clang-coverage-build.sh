#!/usr/bin/env bash
# =============================================================================
# macOS Coverage Build - Host Auto-Detection
# =============================================================================
# Build with Clang coverage instrumentation.
# On macOS: native build.
# On non-macOS: remote build via macBuilder.
#
# Usage:
#   bash ninja-clang-coverage-build.sh
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
            echo "[coverage-build-macos] macOS host detected → native build"
            (
                cd "$KOG_CPP_ROOT"
                cmake --preset macos-ninja-clang-coverage
                cmake --build --preset macos-ninja-clang-coverage-debug
            )
            ;;
        *)
            echo "[coverage-build-macos] Non-macOS host → remote build via macBuilder"
            kog_remote_build_macos "macos-ninja-clang-coverage" "Debug"
            ;;
    esac
}

detect_host_and_build
