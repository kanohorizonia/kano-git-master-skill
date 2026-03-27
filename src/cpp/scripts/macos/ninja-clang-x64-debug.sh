#!/usr/bin/env bash
# =============================================================================
# macOS Build (x64) - Host Auto-Detection
# =============================================================================
# Detects host OS and runs appropriate build:
#   - macOS host → native build
#   - Windows/Linux host → remote build via macBuilder
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/../common/unix_preset_build.sh"
source "$SCRIPT_DIR/../common/macos_remote_build.sh"

detect_host_and_build() {
    local InConfigurePreset="$1"
    local InBuildPreset="$2"
    local host_os
    host_os="$(uname -s 2>/dev/null || true)"

    case "$host_os" in
        Darwin)
            echo "[INFO] macOS host detected → native build"
            export KOG_BUILD_USE_LLVM=1
            kog_run_unix_preset "$InConfigurePreset" "$InBuildPreset"
            ;;
        *)
            echo "[INFO] Non-macOS host detected → remote build via macBuilder"
            kog_remote_build_macos "$InConfigurePreset" "Debug"
            ;;
    esac
}

detect_host_and_build "macos-ninja-clang-x64" "macos-ninja-clang-x64-debug"
