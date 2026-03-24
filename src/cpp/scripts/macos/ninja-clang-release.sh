#!/usr/bin/env bash
# =============================================================================
# macOS Build - Host Auto-Detection
# =============================================================================
# Detects host OS and runs appropriate build:
#   - macOS host → native build
#   - Windows/Linux host → remote build via macBuilder
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
            if [[ -n "$PRIVATE_SCRIPT" ]]; then
                echo "[INFO] Non-macOS host detected → remote build via macBuilder"
                source "$PRIVATE_SCRIPT"
                kog_remote_build_macos "$InConfigurePreset" "Release"
            else
                echo "[ERROR] Private repo not found. Cannot remote build macOS on non-macOS host." >&2
                echo "[ERROR] Expected private script at: $PRIVATE_SCRIPT" >&2
                exit 1
            fi
            ;;
    esac
}

detect_host_and_build "macos-ninja-clang" "macos-ninja-clang-release"