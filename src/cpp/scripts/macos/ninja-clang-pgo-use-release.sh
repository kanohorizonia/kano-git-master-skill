#!/usr/bin/env bash
# =============================================================================
# macOS PGO Use Build - Host Auto-Detection
# =============================================================================
# PGO Phase 2: Build with optimized profile data from PGO collect phase.
# Run this AFTER ninja-clang-pgo-collect-debug.sh and running tests.
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
            echo "[INFO] macOS host detected → native build"
            export KOG_BUILD_USE_LLVM=1
            (
                cd "$KOG_CPP_ROOT"
                cmake --preset macos-ninja-clang-pgo-use
                cmake --build --preset macos-ninja-clang-pgo-use-release
            )
            ;;
        *)
            echo "[INFO] Non-macOS host detected → remote build via macBuilder"
            kog_remote_build_macos "macos-ninja-clang-pgo-use" "Release"
            ;;
    esac
}

detect_host_and_build
