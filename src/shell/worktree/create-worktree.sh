#!/usr/bin/env bash
#
# create-worktree.sh - Create worktree for any branch
#
# Usage:
#   create-worktree.sh <branch> [options]
#
# Examples:
#   create-worktree.sh main
#   create-worktree.sh feature/new --new-branch
#   create-worktree.sh docs --path ../my-docs --open
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/worktree-helpers.sh"

# Default values
BRANCH=""
CUSTOM_PATH=""
NEW_BRANCH=0
OPEN_IDE=0
IDE="auto"
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") <branch> [options]

Create a worktree for any branch.

Arguments:
  branch              Branch name (required)

Options:
  --path <path>       Custom worktree path (default: ../{repo}-{branch})
  --new-branch        Create new branch
  --open              Open in IDE after creation
  --ide <name>        IDE to use: auto, code, idea, vim, terminal (default: auto)
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  # Create worktree for existing branch
  $(basename "$0") main

  # Create worktree with custom path
  $(basename "$0") main --path ../my-main

  # Create worktree and open in IDE
  $(basename "$0") main --open

  # Create worktree for new branch
  $(basename "$0") feature/new --new-branch

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --path)
      CUSTOM_PATH="$2"
      shift 2
      ;;
    --new-branch)
      NEW_BRANCH=1
      shift
      ;;
    --open)
      OPEN_IDE=1
      shift
      ;;
    --ide)
      IDE="$2"
      shift 2
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

# Determine worktree path
if [[ -n "$CUSTOM_PATH" ]]; then
  WORKTREE_PATH="$CUSTOM_PATH"
else
  WORKTREE_PATH=$(wth_generate_worktree_path "$BRANCH")
fi

wth_info "Creating worktree for branch: $BRANCH"
wth_info "Worktree path: $WORKTREE_PATH"

# Check if worktree already exists
if wth_worktree_exists "$BRANCH"; then
  existing_path=$(wth_get_worktree_path "$BRANCH")
  wth_error "Worktree already exists for branch '$BRANCH' at: $existing_path"
  exit 1
fi

# Check if path already exists
if [[ -e "$WORKTREE_PATH" ]]; then
  wth_error "Path already exists: $WORKTREE_PATH"
  exit 1
fi

# Check if branch exists (if not creating new)
if [[ "$NEW_BRANCH" -eq 0 ]]; then
  if ! wth_branch_exists "$BRANCH"; then
    wth_error "Branch '$BRANCH' does not exist. Use --new-branch to create it."
    exit 1
  fi
fi

# Create worktree
if [[ "$DRY_RUN" -eq 1 ]]; then
  if [[ "$NEW_BRANCH" -eq 1 ]]; then
    echo "+ git worktree add -b \"$BRANCH\" \"$WORKTREE_PATH\""
  else
    echo "+ git worktree add \"$WORKTREE_PATH\" \"$BRANCH\""
  fi
else
  if [[ "$NEW_BRANCH" -eq 1 ]]; then
    wth_info "Creating new branch and worktree..."
    git worktree add -b "$BRANCH" "$WORKTREE_PATH"
  else
    wth_info "Creating worktree for existing branch..."
    git worktree add "$WORKTREE_PATH" "$BRANCH"
  fi
  
  wth_info "Worktree created successfully!"
  wth_info "Path: $WORKTREE_PATH"
  wth_info "Branch: $BRANCH"
fi

# Open in IDE if requested
if [[ "$OPEN_IDE" -eq 1 && "$DRY_RUN" -eq 0 ]]; then
  wth_info "Opening in IDE: $IDE"
  if wth_open_in_ide "$WORKTREE_PATH" "$IDE"; then
    wth_info "Opened successfully"
  else
    wth_error "Failed to open in IDE"
    exit 1
  fi
fi

wth_info "Done!"
