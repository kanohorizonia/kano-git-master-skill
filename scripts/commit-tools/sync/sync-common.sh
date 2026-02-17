#!/usr/bin/env bash
#
# sync-common.sh - Shared helpers for sync migration scripts

set -euo pipefail

syncc_resolve_workspace_root() {
  local repo="$1"
  if [[ -n "${KANO_GIT_MASTER_ROOT:-}" ]]; then
    (cd "$KANO_GIT_MASTER_ROOT" && pwd -P)
    return 0
  fi
  git -C "$repo" rev-parse --show-toplevel 2>/dev/null || true
}

syncc_resolve_repo_abs() {
  local repo="$1"
  (cd "$repo" && pwd -P)
}

syncc_resolve_repo_rel_to_root() {
  local repo_abs="$1"
  local root_abs="$2"
  if [[ "$repo_abs" == "$root_abs" ]]; then
    printf '.'
    return 0
  fi
  case "$repo_abs" in
    "$root_abs"/*) printf '%s' "${repo_abs#"$root_abs"/}" ;;
    *) printf '%s' "$repo_abs" ;;
  esac
}

syncc_detect_remote_default_branch() {
  local repo="$1"
  local remote="$2"
  local head_ref=""
  local branch=""

  head_ref="$(git -C "$repo" symbolic-ref --quiet "refs/remotes/$remote/HEAD" 2>/dev/null || true)"
  if [[ -n "$head_ref" ]]; then
    branch="${head_ref#refs/remotes/$remote/}"
    [[ -n "$branch" ]] && printf '%s' "$branch" && return 0
  fi

  for branch in main master dev develop trunk; do
    if git -C "$repo" show-ref --verify --quiet "refs/remotes/$remote/$branch" 2>/dev/null; then
      printf '%s' "$branch"
      return 0
    fi
  done
  return 1
}

syncc_get_gitmodules_branch_for_path() {
  local workspace_root="$1"
  local repo_abs="$2"
  local scan_dir=""

  scan_dir="$(dirname "$repo_abs")"
  while :; do
    local gm_file="$scan_dir/.gitmodules"
    if [[ -f "$gm_file" ]]; then
      local repo_rel_to_scan=""
      local section_key=""
      if [[ "$repo_abs" == "$scan_dir" ]]; then
        repo_rel_to_scan="."
      else
        repo_rel_to_scan="${repo_abs#"$scan_dir"/}"
      fi
      section_key="$(git config -f "$gm_file" --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk -v p="$repo_rel_to_scan" '$2==p {print $1; exit}')"
      if [[ -n "$section_key" ]]; then
        section_key="${section_key%.path}"
        git config -f "$gm_file" --get "$section_key.branch" 2>/dev/null || true
        return 0
      fi
    fi
    [[ "$scan_dir" == "$workspace_root" || "$scan_dir" == "/" ]] && break
    scan_dir="$(dirname "$scan_dir")"
  done
  return 1
}

syncc_set_gitmodules_branch_for_path() {
  local workspace_root="$1"
  local repo_abs="$2"
  local branch="$3"
  local scan_dir=""

  scan_dir="$(dirname "$repo_abs")"
  while :; do
    local gm_file="$scan_dir/.gitmodules"
    if [[ -f "$gm_file" ]]; then
      local repo_rel_to_scan=""
      local section_key=""
      if [[ "$repo_abs" == "$scan_dir" ]]; then
        repo_rel_to_scan="."
      else
        repo_rel_to_scan="${repo_abs#"$scan_dir"/}"
      fi
      section_key="$(git config -f "$gm_file" --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk -v p="$repo_rel_to_scan" '$2==p {print $1; exit}')"
      if [[ -n "$section_key" ]]; then
        section_key="${section_key%.path}"
        git config -f "$gm_file" "$section_key.branch" "$branch"
        printf '%s' "$gm_file"
        return 0
      fi
    fi
    [[ "$scan_dir" == "$workspace_root" || "$scan_dir" == "/" ]] && break
    scan_dir="$(dirname "$scan_dir")"
  done
  return 1
}

syncc_find_gitmodules_file_for_path() {
  local workspace_root="$1"
  local repo_abs="$2"
  local scan_dir=""

  scan_dir="$(dirname "$repo_abs")"
  while :; do
    local gm_file="$scan_dir/.gitmodules"
    if [[ -f "$gm_file" ]]; then
      local repo_rel_to_scan=""
      local section_key=""
      if [[ "$repo_abs" == "$scan_dir" ]]; then
        repo_rel_to_scan="."
      else
        repo_rel_to_scan="${repo_abs#"$scan_dir"/}"
      fi
      section_key="$(git config -f "$gm_file" --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk -v p="$repo_rel_to_scan" '$2==p {print $1; exit}')"
      if [[ -n "$section_key" ]]; then
        printf '%s' "$gm_file"
        return 0
      fi
    fi
    [[ "$scan_dir" == "$workspace_root" || "$scan_dir" == "/" ]] && break
    scan_dir="$(dirname "$scan_dir")"
  done
  return 1
}

syncc_fallback_sync_branch_mode() {
  local repo="$1"
  local origin_remote="$2"
  local workspace_root="$3"
  local repo_abs="$4"
  local dry_run="${5:-0}"
  local branch_from_gitmodules=""
  local target_branch=""

  git -C "$repo" fetch "$origin_remote" --prune >/dev/null 2>&1 || true
  branch_from_gitmodules="$(syncc_get_gitmodules_branch_for_path "$workspace_root" "$repo_abs" || true)"
  if [[ -n "$branch_from_gitmodules" ]]; then
    target_branch="$branch_from_gitmodules"
    echo "Fallback branch source: .gitmodules ($target_branch)"
  else
    target_branch="$(syncc_detect_remote_default_branch "$repo" "$origin_remote" || true)"
    [[ -z "$target_branch" ]] && echo "ERROR: Could not detect default branch for $origin_remote in $repo" >&2 && return 1
    echo "Fallback branch source: remote default branch ($target_branch)"
  fi

  local checkout_cmd=()
  if git -C "$repo" show-ref --verify --quiet "refs/heads/$target_branch"; then
    checkout_cmd=(git -C "$repo" checkout "$target_branch")
  else
    checkout_cmd=(git -C "$repo" checkout -b "$target_branch" "$origin_remote/$target_branch")
  fi
  local pull_cmd=(git -C "$repo" pull --rebase "$origin_remote" "$target_branch")

  if [[ "$dry_run" -eq 1 ]]; then
    echo "[DRY RUN] Fallback sync mode:"
    echo "[DRY RUN] Would run: ${checkout_cmd[*]}"
    echo "[DRY RUN] Would run: ${pull_cmd[*]}"
    return 0
  fi

  "${checkout_cmd[@]}" >/dev/null 2>&1
  "${pull_cmd[@]}"
  echo "Fallback sync complete on branch: $target_branch"
}

syncc_resolve_conflicts_ai() {
  local script_dir="$1"
  local repo="$2"
  local provider="$3"
  local model="$4"
  local resolver=""

  case "$provider" in
    copilot) resolver="$script_dir/../resolve/smart-resolve-copilot.sh" ;;
    codex) resolver="$script_dir/../resolve/smart-resolve-codex.sh" ;;
    opencode) resolver="$script_dir/../resolve/smart-resolve-opencode.sh" ;;
    *) echo "ERROR: Unsupported resolve provider: $provider" >&2; return 1 ;;
  esac

  [[ -f "$resolver" ]] || { echo "ERROR: Resolver script not found: $resolver" >&2; return 1; }
  (
    cd "$repo"
    bash "$resolver" --model "$model" --auto
  )
}

syncc_push_branch() {
  local repo="$1"
  local origin_remote="$2"
  local target_branch="$3"
  local no_push="${4:-0}"
  [[ "$no_push" -eq 1 ]] && echo "Push skipped (--no-push)" && return 0
  git -C "$repo" push -u "$origin_remote" "$target_branch"
}
