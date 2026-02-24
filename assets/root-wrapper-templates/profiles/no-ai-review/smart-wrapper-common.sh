#!/usr/bin/env bash
#
# smart-wrapper-common.sh - Shared helpers for no-ai-review profile wrappers.

set -euo pipefail

resolve_git_master_skill_root() {
  local root="$1"
  local candidate=""
  local candidates=(
    "$root/.agents/skills/kano/kano-git-master-skill"
    "$root/.agents/kano/kano-git-master-skill"
  )

  for candidate in "${candidates[@]}"; do
    if [[ -d "$candidate" ]]; then
      printf '%s' "$candidate"
      return 0
    fi
  done

  echo "ERROR: kano-git-master-skill submodule path not found under $root" >&2
  echo "Tried:" >&2
  echo "  $root/.agents/skills/kano/kano-git-master-skill" >&2
  echo "  $root/.agents/kano/kano-git-master-skill" >&2
  return 1
}

resolve_skill_script_path() {
  local root="$1"
  local rel_path="$2"
  local skill_root

  skill_root="$(resolve_git_master_skill_root "$root")" || return 1
  printf '%s/%s' "$skill_root" "$rel_path"
}

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
  local -a collected=(".")
  local -A seen=()
  local p=""

  add_if_cloned() {
    local rel="$1"
    [[ -z "$rel" ]] && return 0
    if [[ -d "$root/$rel" && ( -f "$root/$rel/.git" || -d "$root/$rel/.git" ) ]]; then
      collected+=("$rel")
    fi
  }

  if [[ ! -f "$root/.gitmodules" ]]; then
    printf '%s' "$repos"
    return 0
  fi

  while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    add_if_cloned "$path"
  done < <(git -C "$root" config --file .gitmodules --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk '{print $2}')

  while IFS= read -r path; do
    [[ -z "$path" ]] && continue
    add_if_cloned "$path"
  done < <(git -C "$root" submodule foreach --quiet --recursive 'printf "%s\n" "$displaypath"' 2>/dev/null || true)

  for p in "${collected[@]}"; do
    if [[ -n "${seen[$p]:-}" ]]; then
      continue
    fi
    seen["$p"]=1
    if [[ "$p" == "." ]]; then
      repos="."
    else
      repos+="${repos:+,}${p}"
    fi
  done

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
