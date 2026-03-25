#!/usr/bin/env bash
# =============================================================================
# macOS Coverage Build - Host Auto-Detection
# =============================================================================
# Build with Clang coverage instrumentation.
# On macOS: native build.
# On non-macOS: remote build via macBuilder (requires private repo).
#
# Usage:
#   bash ninja-clang-coverage-build.sh
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/../common/private_repo_path.sh"

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
            local private_script
            private_script="$(kog_private_script macos_private.sh)"
            if [[ -z "$private_script" || ! -f "$private_script" ]]; then
                echo "[ERROR] Private repo not found. Cannot remote build macOS on non-macOS host." >&2
                exit 1
            fi
            source "$private_script"
            if declare -f kog_remote_build_macos >/dev/null 2>&1; then
                kog_remote_build_macos "macos-ninja-clang-coverage" "Debug"
            else
                echo "[ERROR] kog_remote_build_macos not found in $private_script" >&2
                exit 1
            fi
            ;;
    esac
}

detect_host_and_build