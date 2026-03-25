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

# Try to source private repo for remote build
PRIVATE_SCRIPT=""
source "$SCRIPT_DIR/../common/private_repo_path.sh"
PRIVATE_SCRIPT="$(kog_private_script macos_private.sh)"

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
            if [[ -n "$PRIVATE_SCRIPT" ]]; then
                echo "[INFO] Non-macOS host detected → remote build via macBuilder"
                source "$PRIVATE_SCRIPT"
                kog_remote_build_macos "macos-ninja-clang-pgo-collect" "Debug"
            else
                echo "[ERROR] Private repo not found. Cannot remote build macOS on non-macOS host." >&2
                exit 1
            fi
            ;;
    esac
}

detect_host_and_build