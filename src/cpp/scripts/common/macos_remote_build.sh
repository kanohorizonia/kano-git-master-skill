#!/usr/bin/env bash
# =============================================================================
# macOS Remote Build — kano-remote-host-skill Integration
# =============================================================================
# Uses kano-remote-host-skill to resolve the macOS build host address,
# then performs rsync + SSH build.
#
# Supports two modes:
#   1. kano-remote-host (primary): queries ~/.kano/krh_config.toml for hosts
#   2. Fallback (KOB_MACBUILDER_HOST): backward-compatible hardcoded host
#
# macOS-native builds (when host OS is Darwin) are handled by
# kog_run_unix_preset in unix_preset_build.sh — this script is ONLY for
# remote cross-compilation from Linux/Windows hosts.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOG_REMOTE_HOST_RESOLVER_SH="$SCRIPT_DIR/../../shared/infra/scripts/common/remote_host_resolver.sh"
if [[ -f "$KOG_REMOTE_HOST_RESOLVER_SH" ]]; then
    # shellcheck disable=SC1091
    source "$KOG_REMOTE_HOST_RESOLVER_SH"
fi

# -----------------------------------------------------------------------------
# Default remote build parameters (used when kano-remote-host is unavailable)
# -----------------------------------------------------------------------------
# These can be overridden via environment variables or krh_config.toml.
# KOB_MACBUILDER_HOST defaults to the original hardcoded address for
# backward compatibility during transition.
KOB_MACBUILDER_HOST="${KOB_MACBUILDER_HOST:-dorgon.chang@macbuilder.cobia-tailor.ts.net}"
KOB_REMOTE_BUILD_DIR="${KOB_REMOTE_BUILD_DIR:-/tmp/kano-build}"
KOB_SSH_OPTS="${KOB_SSH_OPTS:--o StrictHostKeyChecking=no -o ConnectTimeout=10}"
KOB_SSH_OPTS_RSYNC="${KOB_SSH_OPTS_RSYNC:-o StrictHostKeyChecking=no -o ConnectTimeout=10}"

# Remote tool search paths (in order of preference)
KOB_CMAKE_SEARCH_PATHS='$HOME/bin/cmake/CMake.app/Contents/bin/cmake /usr/local/bin/cmake /usr/bin/cmake /opt/homebrew/bin/cmake'
KOB_NINJA_SEARCH_PATHS='$HOME/bin/ninja /usr/local/bin/ninja /usr/bin/ninja /opt/homebrew/bin/ninja'

# -----------------------------------------------------------------------------
# Remote build function (called by macOS cross-compile scripts)
# -----------------------------------------------------------------------------
kog_remote_build_macos() {
    local in_configure_preset="${1:-}"
    local in_build_type="${2:-Release}"

    local source_repo="${KOG_CPP_ROOT:-}"
    if [[ -z "$source_repo" ]]; then
        echo "[ERROR] KOG_CPP_ROOT not set" >&2
        return 1
    fi

    # Resolve host address via shared infra helper, then fallback env.
    local host_addr=""
    local host_with_user=""

    if declare -F kano_cpp_pick_remote_host >/dev/null 2>&1; then
        host_with_user="$(kano_cpp_pick_remote_host "${KANO_REMOTE_HOST_GROUP:-mac-local}" "${KANO_REMOTE_HOST_ROUTE:-auto}" "$KOB_MACBUILDER_HOST" || true)"
    fi
    if [[ -z "$host_with_user" ]]; then
        host_with_user="$KOB_MACBUILDER_HOST"
    fi
    host_addr="${host_with_user#*@}"
    echo "[INFO] Using macOS builder: $host_with_user"

    # SSH reachability check
    echo "[INFO] Testing SSH connection to $host_with_user..."
    if ! ssh -o BatchMode=yes -q ${host_with_user:+${host_with_user}} "echo 'SSH OK'" 2>/dev/null; then
        # Try with explicit user@host
        if ! ssh ${KOB_SSH_OPTS} -q "$host_with_user" "echo 'SSH OK'" 2>/dev/null; then
            echo "[ERROR] Cannot connect to $host_with_user" >&2
            return 1
        fi
    fi

    # Rsync source to remote
    echo "[INFO] Rsyncing source to $host_with_user:${KOB_REMOTE_BUILD_DIR}..."
    rsync -avz --delete \
        -e "ssh ${KOB_SSH_OPTS_RSYNC}" \
        --exclude 'out/' \
        --exclude 'build/' \
        --exclude '.git/' \
        --exclude 'node_modules/' \
        --exclude '__pycache__/' \
        --exclude '.kano/' \
        --exclude '.cache/' \
        "${source_repo}/" \
        "${host_with_user}:${KOB_REMOTE_BUILD_DIR}/" 2>&1 | tail -5

    # Detect cmake on remote
    local cmake_path
    cmake_path="$(ssh ${KOB_SSH_OPTS} "$host_with_user" "
        for cmake in ${KOB_CMAKE_SEARCH_PATHS}; do
            if [[ -x \"\$cmake\" ]]; then
                echo \"\$cmake\"
                exit 0
            fi
        done
        echo \"ERROR: cmake not found\" >&2
        exit 1
    ")" || true

    if [[ "$cmake_path" == ERROR:* ]]; then
        echo "[ERROR] $cmake_path" >&2
        return 1
    fi

    # Detect ninja on remote
    local ninja_path
    ninja_path="$(ssh ${KOB_SSH_OPTS} "$host_with_user" "
        for ninja in ${KOB_NINJA_SEARCH_PATHS}; do
            if [[ -x \"\$ninja\" ]]; then
                echo \"\$ninja\"
                exit 0
            fi
        done
        echo \"ERROR: ninja not found\" >&2
        exit 1
    ")" || true

    if [[ "$ninja_path" == ERROR:* ]]; then
        echo "[ERROR] $ninja_path" >&2
        return 1
    fi

    echo "[INFO] Remote tools: cmake=$cmake_path ninja=$ninja_path"

    # Build on remote
    local build_preset="${in_configure_preset}-${in_build_type,,}"
    echo "[INFO] Building preset='$in_configure_preset' type='$in_build_type' (build preset='$build_preset')..."

    ssh ${KOB_SSH_OPTS} "$host_with_user" "
        set -euo pipefail
        export PATH=\"$(dirname "$cmake_path"):$(dirname "$ninja_path"):\$PATH\"
        cd '${KOB_REMOTE_BUILD_DIR}'
        rm -rf out
        '$cmake_path' --preset '${in_configure_preset}'
        '$cmake_path' --build --preset '${build_preset}' -j\$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
    "

    echo "[INFO] Build complete"
}
