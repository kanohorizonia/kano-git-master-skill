#!/usr/bin/env bash
#
# worktree-helpers.sh - Shared helper functions for Git worktree operations
#
# This library provides common functions for worktree management scripts.
# All functions are prefixed with 'wth_' (worktree-helper).
#
# Usage:
#   source "$(dirname "${BASH_SOURCE[0]}")/worktree-helpers.sh"
#

set -euo pipefail

# Version
WTH_VERSION="1.0.0"

# Generate worktree path following convention: ../{repo}-{branch}
# Args:
#   $1 - branch name
# Returns:
#   Generated worktree path
wth_generate_worktree_path() {
  local branch_name="$1"
  local repo_name
  
  repo_name=$(basename "$(git rev-parse --show-toplevel)")
  
  # Sanitize branch name for filesystem (replace / with -)
  local safe_branch="${branch_name//\//-}"
  
  echo "../${repo_name}-${safe_branch}"
}

# Check if branch is an orphan branch (has no parent commits)
# Args:
#   $1 - branch name
# Returns:
#   0 if orphan, 1 if not
wth_is_orphan_branch() {
  local branch="$1"
  local parent_count
  
  # Count parent commits (orphan branches have only 1 word in rev-list --parents output)
  parent_count=$(git rev-list --parents "$branch" 2>/dev/null | tail -1 | wc -w)
  
  [[ "$parent_count" -eq 1 ]]
}

# Check if worktree already exists for a branch
# Args:
#   $1 - branch name
# Returns:
#   0 if exists, 1 if not
wth_worktree_exists() {
  local branch="$1"
  
  git worktree list | grep -q "\\[$branch\\]"
}

# Get worktree path for a branch
# Args:
#   $1 - branch name
# Returns:
#   Worktree path if exists, empty if not
wth_get_worktree_path() {
  local branch="$1"
  
  git worktree list | grep "\\[$branch\\]" | awk '{print $1}' || true
}

# Check if branch exists locally
# Args:
#   $1 - branch name
# Returns:
#   0 if exists, 1 if not
wth_branch_exists() {
  local branch="$1"
  
  git show-ref --verify --quiet "refs/heads/$branch"
}

# Check if branch exists on remote
# Args:
#   $1 - remote name
#   $2 - branch name
# Returns:
#   0 if exists, 1 if not
wth_branch_exists_on_remote() {
  local remote="$1"
  local branch="$2"
  
  git show-ref --verify --quiet "refs/remotes/$remote/$branch"
}

# Open path in IDE
# Args:
#   $1 - path to open
#   $2 - IDE name (auto, code, idea, vim, terminal)
# Returns:
#   0 on success, 1 on failure
wth_open_in_ide() {
  local path="$1"
  local ide="${2:-auto}"
  
  case "$ide" in
    auto)
      if command -v code &>/dev/null; then
        code "$path"
      elif command -v idea &>/dev/null; then
        idea "$path"
      else
        echo "No IDE found, opening in terminal"
        cd "$path" && ${SHELL:-bash}
      fi
      ;;
    code|vscode)
      if command -v code &>/dev/null; then
        code "$path"
      else
        echo "Error: VS Code not found" >&2
        return 1
      fi
      ;;
    idea|intellij)
      if command -v idea &>/dev/null; then
        idea "$path"
      else
        echo "Error: IntelliJ IDEA not found" >&2
        return 1
      fi
      ;;
    vim|nvim)
      cd "$path" && ${EDITOR:-vim} .
      ;;
    terminal)
      cd "$path" && ${SHELL:-bash}
      ;;
    *)
      echo "Error: Unknown IDE: $ide" >&2
      return 1
      ;;
  esac
}

# Get last commit info for a worktree
# Args:
#   $1 - worktree path
# Returns:
#   Formatted commit info: "hash message"
wth_get_last_commit() {
  local worktree_path="$1"
  
  (cd "$worktree_path" && git log -1 --format="%h %s" 2>/dev/null) || echo "N/A"
}

# Check if worktree has uncommitted changes
# Args:
#   $1 - worktree path
# Returns:
#   0 if has changes, 1 if clean
wth_has_changes() {
  local worktree_path="$1"
  
  [[ -n "$(cd "$worktree_path" && git status --porcelain 2>/dev/null)" ]]
}

# Get worktree status (Clean, Modified, Untracked)
# Args:
#   $1 - worktree path
# Returns:
#   Status string
wth_get_status() {
  local worktree_path="$1"
  
  if ! wth_has_changes "$worktree_path"; then
    echo "Clean"
  else
    local status_output
    status_output=$(cd "$worktree_path" && git status --porcelain 2>/dev/null)
    
    if echo "$status_output" | grep -q "^??"; then
      echo "Untracked"
    else
      echo "Modified"
    fi
  fi
}

# List all worktrees with metadata
# Returns:
#   JSON array of worktree objects
wth_list_worktrees_json() {
  local worktrees
  worktrees=$(git worktree list --porcelain)
  
  echo "["
  local first=1
  local path="" branch="" commit=""
  
  while IFS= read -r line; do
    if [[ "$line" =~ ^worktree\ (.+)$ ]]; then
      path="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^branch\ refs/heads/(.+)$ ]]; then
      branch="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^HEAD\ ([a-f0-9]+)$ ]]; then
      commit="${BASH_REMATCH[1]}"
    elif [[ -z "$line" && -n "$path" ]]; then
      # End of worktree entry
      if [[ "$first" -eq 0 ]]; then
        echo ","
      fi
      first=0
      
      local is_orphan="false"
      if [[ -n "$branch" ]] && wth_is_orphan_branch "$branch"; then
        is_orphan="true"
      fi
      
      local status
      status=$(wth_get_status "$path")
      
      local last_commit
      last_commit=$(wth_get_last_commit "$path")
      
      echo "  {"
      echo "    \"path\": \"$path\","
      echo "    \"branch\": \"${branch:-detached}\","
      echo "    \"commit\": \"$commit\","
      echo "    \"orphan\": $is_orphan,"
      echo "    \"status\": \"$status\","
      echo "    \"last_commit\": \"$last_commit\""
      echo -n "  }"
      
      path=""
      branch=""
      commit=""
    fi
  done <<< "$worktrees"
  
  echo ""
  echo "]"
}

# Log message with timestamp
# Args:
#   $1 - message
wth_log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

# Log error message
# Args:
#   $1 - error message
wth_error() {
  echo "[ERROR] $*" >&2
}

# Log info message
# Args:
#   $1 - info message
wth_info() {
  echo "[INFO] $*"
}
