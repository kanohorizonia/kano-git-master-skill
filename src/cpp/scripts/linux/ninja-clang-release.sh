#!/usr/bin/env bash
# =============================================================================
# Linux Build - Host Auto-Detection
# =============================================================================
# Detects host OS and runs appropriate build:
#   - Linux host → native build
#   - Windows/macOS host + Docker → Docker build
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/../common/unix_preset_build.sh"
source "$SCRIPT_DIR/../common/docker_linux_build.sh"

detect_host_and_build() {
    local InConfigurePreset="$1"
    local InBuildPreset="$2"
    local host_os
    host_os="$(uname -s 2>/dev/null || true)"

    case "$host_os" in
        Linux)
            echo "[INFO] Linux host detected → native build"
            kog_run_unix_preset "$InConfigurePreset" "$InBuildPreset"
            ;;
        Darwin|MINGW*|MSYS*|CYGWIN*)
            if command -v docker >/dev/null 2>&1; then
                echo "[INFO] Non-Linux host + Docker detected → Docker build"
                kog_run_linux_preset_via_docker "$InConfigurePreset" "$InBuildPreset"
            else
                echo "[ERROR] Docker required for Linux builds on non-Linux host" >&2
                exit 1
            fi
            ;;
        *)
            echo "[ERROR] Unknown host OS: $host_os" >&2
            exit 1
            ;;
    esac
}

detect_host_and_build "linux-ninja-clang" "linux-ninja-clang-release"