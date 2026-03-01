#!/usr/bin/env bash
#
# remove-worktree.sh - Safely remove a worktree
#
# Usage:
#   remove-worktree.sh <branch> [options]
#
# Examples:
#   remove-worktree.sh docs
#   remove-worktree.sh docs --force
#   remove-worktree.sh docs --delete-branch
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/worktree-helpers.sh"

# Default values
BRANCH=""
FORCE=0
DELETE_BRANCH=0
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") <branch> [options]

Safely remove a worktree.

Arguments:
  branch              Branch name (required)

Options:
  --force             Force removal even with uncommitted changes
  --delete-branch     Also delete the branch
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  # Remove worktree
  $(basename "$0") docs

  # Force remove (even with uncommitted changes)
  $(basename "$0") docs --force

  # Remove and delete branch
  $(basename "$0") docs --delete-branch

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --force)
      FORCE=1
      shift
      ;;
    --delete-branch)
      DELETE_BRANCH=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -*)
      echo "Error: Unknown option: $1" >&2
      usage
      exit 1
      ;;
    *)
      if [[ -z "$BRANCH" ]]; then
        BRANCH="$1"
      else
        echo "Error: Unexpected argument: $1" >&2
        usage
        exit 1
      fi
      shift
      ;;
  esac
done

# Validate required arguments
if [[ -z "$BRANCH" ]]; then
  echo "Error: Branch name is required" >&2
  usage
  exit 1
fi

# Check if we're in a git repository
if ! git rev-parse --git-dir &>/dev/null; then
  wth_error "Not in a git repository"
  exit 1
fi

# Check if worktree exists
if ! wth_worktree_exists "$BRANCH"; then
  wth_error "Worktree does not exist for branch: $BRANCH"
  exit 1
fi

# Get worktree path
WORKTREE_PATH=$(wth_get_worktree_path "$BRANCH")

wth_info "Removing worktree for branch: $BRANCH"
wth_info "Worktree path: $WORKTREE_PATH"

# Check for uncommitted changes
if [[ "$FORCE" -eq 0 ]] && wth_has_changes "$WORKTREE_PATH"; then
  wth_error "Worktree has uncommitted changes. Use --force to remove anyway."
  wth_info "Changed files:"
  (cd "$WORKTREE_PATH" && git status --short)
  exit 1
fi

# Warn before deletion
if [[ "$DRY_RUN" -eq 0 && "$FORCE" -eq 0 ]]; then
  wth_info "WARNING: This will remove the worktree directory and all its contents."
  wth_info "Press Ctrl+C to cancel, or wait 3 seconds to continue..."
  sleep 3
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "+ git worktree remove \"$WORKTREE_PATH\""
  if [[ "$DELETE_BRANCH" -eq 1 ]]; then
    echo "+ git branch -D \"$BRANCH\""
  fi
else
  # Remove worktree
  wth_info "Removing worktree..."
  if [[ "$FORCE" -eq 1 ]]; then
    git worktree remove --force "$WORKTREE_PATH"
  else
    git worktree remove "$WORKTREE_PATH"
  fi
  
  wth_info "Worktree removed successfully!"
  
  # Delete branch if requested
  if [[ "$DELETE_BRANCH" -eq 1 ]]; then
    wth_info "Deleting branch: $BRANCH"
    git branch -D "$BRANCH"
    wth_info "Branch deleted successfully!"
  fi
fi

wth_info "Done!"
