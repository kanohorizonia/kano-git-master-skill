#!/usr/bin/env bash
# =============================================================================
# macOS PGO Collect Build - Host Auto-Detection
# =============================================================================
# PGO Phase 1: Build with instrumentation to collect profile data.
# After this, run tests, then build with ninja-clang-pgo-use-release.sh
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
                cmake --preset macos-ninja-clang-pgo-collect
                cmake --build --preset macos-ninja-clang-pgo-collect-debug
            )
            ;;
        *)
            echo "[INFO] Non-macOS host detected → remote build via macBuilder"
            kog_remote_build_macos "macos-ninja-clang-pgo-collect" "Debug"
            ;;
    esac
}

detect_host_and_build
