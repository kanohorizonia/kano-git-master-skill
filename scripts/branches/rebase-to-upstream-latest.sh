#!/usr/bin/env bash
#
# rebase-to-upstream-latest.sh - Rebase to upstream's latest branch
#
# Purpose:
#   Rebase the current local branch onto the latest upstream branch across
#   root repository and all submodules (recursive).
#
# Usage:
#   ./rebase-to-upstream-latest.sh [options]
#
# Options:
#   --branch <name>              Base branch name (default: main)
#   --remote <name>              Remote name (default: upstream)
#   --detached <checkout|skip>   What to do when HEAD is detached
#   --no-stash                   Fail if there are local changes
#   -h, --help                   Show help
#
# Default behavior:
#   - Fetch from upstream/<branch> (default: upstream/main)
#   - If HEAD is on a branch: git rebase upstream/<branch>
#   - If HEAD is detached: checkout <branch> at upstream/<branch> (default)
#   - Auto-stash local changes (including untracked) before operations, pop after success
#   - Does NOT push
#
# Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.)
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

#------------------------------------------------------------------------------
# Configuration
#------------------------------------------------------------------------------

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
fi

BRANCH="main"
REMOTE="upstream"
DETACHED_MODE="checkout" # checkout|skip
AUTO_STASH=1

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: rebase-to-upstream-latest.sh [options]

Rebase the current local branch onto the latest upstream branch across:
  1) root repo
  2) all submodules (recursive)

Default behavior:
  - Fetch upstream/<branch> (default: upstream/main)
  - If HEAD is on a branch: git rebase upstream/<branch>
  - If HEAD is detached: checkout <branch> at upstream/<branch> (default)
  - Auto-stash local changes (including untracked) before operations, pop after success
  - Does NOT push

Options:
  --branch <name>              Base branch name (default: main)
  --remote <name>              Remote name (default: upstream)
  --detached <checkout|skip>   What to do when HEAD is detached (default: checkout)
  --no-stash                   Fail if there are local changes (no auto-stash)
  -h, --help                   Show this help

Examples:
  # Rebase to upstream/main
  ./rebase-to-upstream-latest.sh

  # Rebase to upstream/develop
  ./rebase-to-upstream-latest.sh --branch develop

  # Rebase to origin/main (instead of upstream)
  ./rebase-to-upstream-latest.sh --remote origin

Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.)
EOF
}

# Add repository to processing list
add_repo() {
  local repo="$1"
  if [[ -z "$repo" ]]; then
    return
  fi
  if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    return
  fi
  repo="$(cd "$repo" && pwd)"
  if grep -Fxq "$repo" "$REPO_LIST_FILE" 2>/dev/null; then
    return
  fi
  printf '%s\n' "$repo" >>"$REPO_LIST_FILE"
  REPOS+=("$repo")
}

# Process a single repository
process_repo() {
  local repo="$1"
  local stash_created=0
  local stash_ref=""

  gith_log "INFO" "Processing: $repo"

  # Check if remote exists
  if ! gith_has_remote "$REMOTE" "$repo"; then
    gith_log "WARN" "Skip: '$REMOTE' remote not found in $repo"
    return 0
  fi

  # Fetch from remote
  gith_log "INFO" "Fetching '$REMOTE/$BRANCH'..."
  if ! gith_fetch_remote "$REMOTE" "$repo"; then
    gith_error "Failed to fetch from $REMOTE"
    return 1
  fi

  # Check if remote branch exists
  if ! gith_branch_exists_on_remote "$REMOTE" "$BRANCH" "$repo"; then
    gith_log "WARN" "Skip: '$REMOTE/$BRANCH' does not exist in $repo"
    return 0
  fi

  # Handle local changes
  if gith_has_changes "$repo"; then
    if [[ "$AUTO_STASH" -eq 0 ]]; then
      gith_error "Local changes detected; re-run without --no-stash or clean the working tree"
      return 1
    fi
    
    gith_log "INFO" "Local changes detected; stashing before rebase..."
    stash_ref="$(gith_stash_create "$repo" "auto-stash: rebase-to-upstream-latest")"
    if [[ $? -eq 0 ]] && [[ -n "$stash_ref" ]]; then
      stash_created=1
    fi
  fi

  # Get current branch
  local start_branch
  start_branch="$(gith_get_current_branch "$repo")"
  
  if [[ -z "$start_branch" ]]; then
    # Detached HEAD
    if [[ "$DETACHED_MODE" == "skip" ]]; then
      gith_log "WARN" "Skip: HEAD is detached (use --detached checkout to update)"
      return 0
    fi
    
    gith_log "INFO" "HEAD is detached; checking out '$BRANCH' at '$REMOTE/$BRANCH'..."
    if ! (cd "$repo" && git checkout -B "$BRANCH" "refs/remotes/$REMOTE/$BRANCH" 2>&1); then
      gith_error "Failed to checkout branch"
      if [[ $stash_created -eq 1 ]]; then
        gith_log "INFO" "Stash preserved: $stash_ref"
      fi
      return 1
    fi
  else
    # On a branch - rebase
    gith_log "INFO" "Rebasing '$start_branch' onto '$REMOTE/$BRANCH'..."
    if ! (cd "$repo" && git rebase "refs/remotes/$REMOTE/$BRANCH" 2>&1); then
      gith_error "Rebase failed"
      if [[ $stash_created -eq 1 ]]; then
        gith_log "INFO" "Stash preserved: $stash_ref"
        gith_log "INFO" "After resolving conflicts:"
        gith_log "INFO" "  git rebase --continue"
        gith_log "INFO" "  git stash pop $stash_ref"
      fi
      return 1
    fi
  fi

  # Restore stash
  if [[ "$stash_created" -eq 1 ]]; then
    gith_log "INFO" "Restoring stashed local changes..."
    if ! gith_stash_pop "$repo" "$stash_ref"; then
      gith_error "Auto stash pop failed"
      gith_error "Resolve conflicts and apply manually from $stash_ref"
      return 1
    fi
  fi

  return 0
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --branch)
        BRANCH="${2:-}"
        shift 2
        ;;
      --remote)
        REMOTE="${2:-}"
        shift 2
        ;;
      --detached)
        DETACHED_MODE="${2:-}"
        shift 2
        ;;
      --no-stash)
        AUTO_STASH=0
        shift
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      *)
        gith_error "Unknown argument: $1"
        usage
        exit 1
        ;;
    esac
  done

  # Validate arguments
  if [[ -z "$BRANCH" ]]; then
    gith_error "--branch requires a value"
    exit 2
  fi

  if [[ -z "$REMOTE" ]]; then
    gith_error "--remote requires a value"
    exit 2
  fi

  if [[ "$DETACHED_MODE" != "checkout" && "$DETACHED_MODE" != "skip" ]]; then
    gith_error "--detached must be 'checkout' or 'skip'"
    exit 2
  fi

  # Validate root is a git repository
  if ! git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    gith_error "Not a git repository: $ROOT"
    exit 1
  fi

  # Collect repositories
  declare -a REPOS=()
  REPO_LIST_FILE="$(mktemp -t rebase-to-upstream-latest.repos.XXXXXX)"
  trap 'rm -f "$REPO_LIST_FILE"' EXIT
  touch "$REPO_LIST_FILE"

  # Add root repository
  add_repo "$ROOT"

  # Add submodules
  if [[ -f "$ROOT/.gitmodules" ]]; then
    while IFS= read -r sm_path; do
      [[ -n "$sm_path" ]] || continue
      add_repo "$ROOT/$sm_path"
    done < <(git -C "$ROOT" submodule foreach --recursive --quiet 'echo "$sm_path"' 2>/dev/null || true)
  fi

  gith_log "INFO" "Root: $ROOT"
  gith_log "INFO" "Repos: ${#REPOS[@]} (root + submodules)"
  gith_log "INFO" "Remote: $REMOTE"
  gith_log "INFO" "Branch: $BRANCH"

  # Process all repositories
  local failures=0
  for repo in "${REPOS[@]}"; do
    if ! process_repo "$repo"; then
      failures=$((failures + 1))
    fi
  done

  # Report results
  if [[ "$failures" -ne 0 ]]; then
    gith_error "$failures repo(s) failed. Fix conflicts/errors above, then re-run."
    exit 1
  fi

  gith_log "INFO" "Done."
  exit 0
}

# Run main function
main "$@"
