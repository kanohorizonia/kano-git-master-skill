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

# Try to source private repo for remote build
PRIVATE_SCRIPT=""
if [[ -f "/c/Users/dorgon.chang/.agents/skills/kano-git-master-skill-private/scripts/macos_private.sh" ]]; then
    PRIVATE_SCRIPT="/c/Users/dorgon.chang/.agents/skills/kano-git-master-skill-private/scripts/macos_private.sh"
elif [[ -f "$HOME/.agents/skills/kano-git-master-skill-private/scripts/macos_private.sh" ]]; then
    PRIVATE_SCRIPT="$HOME/.agents/skills/kano-git-master-skill-private/scripts/macos_private.sh"
fi

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
            if [[ -n "$PRIVATE_SCRIPT" ]]; then
                echo "[INFO] Non-macOS host detected → remote build via macBuilder"
                source "$PRIVATE_SCRIPT"
                kog_remote_build_macos "macos-ninja-clang-pgo-use" "Release"
            else
                echo "[ERROR] Private repo not found. Cannot remote build macOS on non-macOS host." >&2
                exit 1
            fi
            ;;
    esac
}

detect_host_and_build