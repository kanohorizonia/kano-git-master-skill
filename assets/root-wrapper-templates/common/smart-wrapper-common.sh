#!/usr/bin/env bash
#
# smart-wrapper-common.sh - Shared helpers for root wrapper templates.

set -euo pipefail

ensure_skill_script_exists() {
  local script="$1"
  if [[ -f "$script" ]]; then
    return 0
  fi
  echo "ERROR: Git Master Skill script not found at:" >&2
  echo "  $script" >&2
  echo "Ensure the kano-git-master-skill submodule is initialized." >&2
  return 1
}

run_skill_script_from_root() {
  local root="$1"
  local script="$2"
  shift 2 || true

  export KANO_GIT_MASTER_ROOT="$root"
  (
    cd "$root"
    bash "$script" "$@"
  )
}

pause_if_needed() {
  # Pause only for interactive human runs.
  [[ -t 0 && -t 1 ]] || return 0
  [[ "${CI:-}" == "1" || "${CI:-}" == "true" ]] && return 0

  local args=("$@")
  local i=0
  local arg=""
  local agent=""
  for ((i=0; i<${#args[@]}; i++)); do
    arg="${args[$i]}"
    case "$arg" in
      --agent)
        if [[ $((i+1)) -lt ${#args[@]} ]]; then
          agent="${args[$((i+1))]}"
        fi
        ;;
      --agent=*)
        agent="${arg#--agent=}"
        ;;
    esac
  done

  if [[ -n "$agent" ]]; then
    agent="$(printf '%s' "$agent" | tr '[:upper:]' '[:lower:]')"
    if [[ "$agent" != "manual" ]]; then
      return 0
    fi
  fi

  read -r -p "Press Enter to continue..."
}

