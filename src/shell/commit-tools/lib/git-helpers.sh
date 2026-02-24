#!/usr/bin/env bash
#
# git-helpers.sh - Common Git helper functions
#
# Purpose:
#   Shared Git utility functions for smart-* scripts
#

set -euo pipefail

#------------------------------------------------------------------------------
# Branch Operations
#------------------------------------------------------------------------------

get_current_branch() {
  local repo="${1:-.}"
  git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || echo ""
}

get_upstream_branch() {
  local repo="${1:-.}"
  git -C "$repo" rev-parse --abbrev-ref "@{upstream}" 2>/dev/null || echo ""
}

has_upstream() {
  local repo="${1:-.}"
  [[ -n "$(get_upstream_branch "$repo")" ]]
}

#------------------------------------------------------------------------------
# Repository State
#------------------------------------------------------------------------------

is_clean_working_tree() {
  local repo="${1:-.}"
  git -C "$repo" diff-index --quiet HEAD 2>/dev/null
}

has_staged_changes() {
  local repo="${1:-.}"
  ! git -C "$repo" diff --cached --quiet 2>/dev/null
}

has_unstaged_changes() {
  local repo="${1:-.}"
  ! git -C "$repo" diff --quiet 2>/dev/null
}

is_detached_head() {
  local repo="${1:-.}"
  ! git -C "$repo" symbolic-ref --quiet HEAD >/dev/null 2>&1
}

#------------------------------------------------------------------------------
# Conflict Detection
#------------------------------------------------------------------------------

has_conflicts() {
  local repo="${1:-.}"
  git -C "$repo" diff --name-only --diff-filter=U 2>/dev/null | grep -q .
}

get_conflicted_files() {
  local repo="${1:-.}"
  git -C "$repo" diff --name-only --diff-filter=U 2>/dev/null || true
}

count_conflicts() {
  local repo="${1:-.}"
  git -C "$repo" diff --name-only --diff-filter=U 2>/dev/null | wc -l
}

#------------------------------------------------------------------------------
# Merge/Rebase State
#------------------------------------------------------------------------------

is_merge_in_progress() {
  local repo="${1:-.}"
  local merge_head
  merge_head="$(git -C "$repo" rev-parse --git-path MERGE_HEAD 2>/dev/null || true)"
  [[ -f "$merge_head" ]]
}

is_rebase_in_progress() {
  local repo="${1:-.}"
  local rebase_merge rebase_apply
  rebase_merge="$(git -C "$repo" rev-parse --git-path rebase-merge 2>/dev/null || true)"
  rebase_apply="$(git -C "$repo" rev-parse --git-path rebase-apply 2>/dev/null || true)"
  [[ -d "$rebase_merge" || -d "$rebase_apply" ]]
}

is_cherry_pick_in_progress() {
  local repo="${1:-.}"
  local cherry_pick_head
  cherry_pick_head="$(git -C "$repo" rev-parse --git-path CHERRY_PICK_HEAD 2>/dev/null || true)"
  [[ -f "$cherry_pick_head" ]]
}

get_operation_in_progress() {
  local repo="${1:-.}"

  if is_merge_in_progress "$repo"; then
    echo "merge"
  elif is_rebase_in_progress "$repo"; then
    echo "rebase"
  elif is_cherry_pick_in_progress "$repo"; then
    echo "cherry-pick"
  else
    echo ""
  fi
}

#------------------------------------------------------------------------------
# Commit Information
#------------------------------------------------------------------------------

get_commit_count() {
  local repo="${1:-.}"
  local base="${2:-HEAD}"
  git -C "$repo" rev-list --count "$base" 2>/dev/null || echo "0"
}

get_commit_message() {
  local repo="${1:-.}"
  local commit="${2:-HEAD}"
  git -C "$repo" log -1 --pretty=%B "$commit" 2>/dev/null || echo ""
}

get_commit_author() {
  local repo="${1:-.}"
  local commit="${2:-HEAD}"
  git -C "$repo" log -1 --pretty=%an "$commit" 2>/dev/null || echo ""
}

#------------------------------------------------------------------------------
# Remote Operations
#------------------------------------------------------------------------------

has_remote() {
  local repo="${1:-.}"
  local remote="${2:-origin}"
  git -C "$repo" remote | grep -q "^${remote}$"
}

get_remote_url() {
  local repo="${1:-.}"
  local remote="${2:-origin}"
  git -C "$repo" remote get-url "$remote" 2>/dev/null || echo ""
}

#------------------------------------------------------------------------------
# Validation
#------------------------------------------------------------------------------

is_git_repo() {
  local repo="${1:-.}"
  git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1
}

validate_repo() {
  local repo="${1:-.}"

  if ! is_git_repo "$repo"; then
    echo "ERROR: Not a git repository: $repo" >&2
    return 1
  fi

  return 0
}
