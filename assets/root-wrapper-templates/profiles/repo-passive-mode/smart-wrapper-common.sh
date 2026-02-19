#!/usr/bin/env bash
#
# smart-wrapper-common.sh - Shared helpers for repo-passive-mode profile wrappers.

set -euo pipefail

ensure_skill_script_exists() {
  local script="$1"
  if [[ -f "$script" ]]; then
    return 0
  fi
  echo "ERROR: Git Master Skill script not found at:" >&2
  echo "  $script" >&2
  echo "This wrapper is passive by design and will not auto-init/clone submodules." >&2
  echo "Initialize required submodules manually on this device first." >&2
  return 1
}

has_arg() {
  local needle="$1"
  shift
  local arg=""
  for arg in "$@"; do
    if [[ "$arg" == "$needle" ]]; then
      return 0
    fi
  done
  return 1
}

collect_cloned_repos_csv() {
  local root="$1"
  local repos="."
  local path=""

  if [[ ! -f "$root/.gitmodules" ]]; then
    printf '%s' "$repos"
    return 0
  fi

  while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    if [[ -d "$root/$path" && ( -f "$root/$path/.git" || -d "$root/$path/.git" ) ]]; then
      repos="${repos},${path}"
    fi
  done < <(git -C "$root" config --file .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk '{print $2}')

  printf '%s' "$repos"
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
