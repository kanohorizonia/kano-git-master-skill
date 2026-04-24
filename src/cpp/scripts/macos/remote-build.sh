#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck disable=SC1091
source "$SCRIPT_DIR/../../shared/infra/scripts/lib/macos_remote_build.sh"
# shellcheck disable=SC1091
source "$SCRIPT_DIR/../../shared/infra/scripts/lib/common-remote.sh"

kog_remote_artifact_allowlist() {
    local artifact_policy="${1:-build}"
    local configure_preset="${2:-}"
    local build_type="${3:-Release}"
    local config_dir="${build_type,,}"

    case "$artifact_policy" in
        build|default)
            if [[ -z "$configure_preset" ]]; then
                echo "kog_remote_artifact_allowlist requires configure_preset for build policy" >&2
                return 1
            fi
            printf '%s\n' "out/bin/${configure_preset}/${config_dir}/"
            printf '%s\n' "out/lib/${configure_preset}/${config_dir}/"
            ;;
        coverage)
            printf '%s\n' "out/coverage/"
            ;;
        *)
            echo "Unsupported artifact policy: $artifact_policy" >&2
            return 1
            ;;
    esac
}

kog_remote_build_macos() {
    local configure_preset="${1:-}"
    local build_type="${2:-Release}"

    export KANO_REMOTE_HOST_GROUP="${KANO_REMOTE_HOST_GROUP:-${KOG_REMOTE_HOST_GROUP:-mac-local}}"
    export KANO_REMOTE_HOST_ROUTE="${KANO_REMOTE_HOST_ROUTE:-${KOG_REMOTE_HOST_ROUTE:-auto}}"

    if [[ -z "${KANO_REMOTE_BUILD_FALLBACK_HOST:-}" && -n "${KOG_REMOTE_MAC_FALLBACK_HOST:-}" ]]; then
        export KANO_REMOTE_BUILD_FALLBACK_HOST="$KOG_REMOTE_MAC_FALLBACK_HOST"
    fi

    inf_remote_build_macos "$configure_preset" "$build_type"
}

kog_remote_sync_back_macos_artifacts() {
    local remote_host="${1:-}"
    local remote_root="${2:-}"
    local local_root="${3:-}"
    shift 3 || true

    if [[ -z "$remote_host" || -z "$remote_root" || -z "$local_root" ]]; then
        echo "kog_remote_sync_back_macos_artifacts requires remote_host, remote_root, and local_root" >&2
        return 1
    fi
    if [[ "$#" -eq 0 ]]; then
        echo "kog_remote_sync_back_macos_artifacts requires at least one allowlisted artifact path" >&2
        return 1
    fi

    mkdir -p "$local_root"

    local ssh_opts_rsync="${KANO_REMOTE_BUILD_SSH_OPTS_RSYNC:-${KOB_SSH_OPTS_RSYNC:--o StrictHostKeyChecking=no -o ConnectTimeout=10}}"
    local rsync_cmd=""
    rsync_cmd="$(horizon_base_resolve_rsync_cmd)"
    local rsync_protocol_flag=""
    rsync_protocol_flag="$(horizon_base_rsync_protocol_flag "$remote_host" || echo "")"
    local artifact_path=""
    for artifact_path in "$@"; do
        echo "[INFO] Artifact sync-back -> ${artifact_path}"
        "$rsync_cmd" -avz \
            -e "ssh ${ssh_opts_rsync}" \
            ${rsync_protocol_flag} \
            --relative \
            "${remote_host}:${remote_root%/}/./${artifact_path}" \
            "${local_root%/}/"
    done
}

kog_remote_sync_back_macos_policy() {
    local remote_host="${1:-}"
    local remote_root="${2:-}"
    local local_root="${3:-}"
    local artifact_policy="${4:-build}"
    local configure_preset="${5:-}"
    local build_type="${6:-Release}"

    if [[ -z "$remote_host" || -z "$remote_root" || -z "$local_root" ]]; then
        echo "kog_remote_sync_back_macos_policy requires remote_host, remote_root, and local_root" >&2
        return 1
    fi

    local artifacts=()
    while IFS= read -r artifact_path; do
        [[ -n "$artifact_path" ]] && artifacts+=("$artifact_path")
    done < <(kog_remote_artifact_allowlist "$artifact_policy" "$configure_preset" "$build_type")

    if [[ "${#artifacts[@]}" -eq 0 ]]; then
        echo "No artifacts defined for policy: $artifact_policy" >&2
        return 1
    fi

    kog_remote_sync_back_macos_artifacts "$remote_host" "$remote_root" "$local_root" "${artifacts[@]}"
}
