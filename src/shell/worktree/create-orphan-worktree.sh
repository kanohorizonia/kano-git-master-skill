#!/usr/bin/env bash
#
# create-orphan-worktree.sh - Create orphan branch with worktree
#
# Usage:
#   create-orphan-worktree.sh <branch> [options]
#
# Examples:
#   create-orphan-worktree.sh docs
#   create-orphan-worktree.sh gh-pages --file index.html --open
#   create-orphan-worktree.sh docs --path ~/projects/repo-docs
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/worktree-helpers.sh"

# Default values
BRANCH=""
CUSTOM_PATH=""
INITIAL_FILE="README.md"
INITIAL_CONTENT=""
COMMIT_MESSAGE="Initial commit"
OPEN_IDE=0
IDE="auto"
PUSH_REMOTE=0
FORCE_OVERWRITE=0
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") <branch> [options]

Create an orphan branch with a worktree in one step.

Arguments:
  branch                  Orphan branch name (required)

Options:
  --path <path>           Custom worktree path (default: ../{repo}-{branch})
  --file <name>           Initial file name (default: README.md)
  --content <text>        File content (default: "# {branch}")
  --message <text>        Commit message (default: "Initial commit")
  --open                  Open in IDE after creation
  --ide <name>            IDE to use: auto, code, idea, vim, terminal (default: auto)
  --push                  Push to remote after creation
  --force-overwrite-branch  Overwrite existing branch (DANGEROUS)
  --dry-run               Show what would be done
  -h, --help              Show this help

Examples:
  # Create orphan branch + worktree for docs
  $(basename "$0") docs

  # With custom content
  $(basename "$0") docs --file README.md --content "# Documentation"

  # Create and open in IDE
  $(basename "$0") gh-pages --file index.html --content "<h1>Site</h1>" --open

  # With custom path
  $(basename "$0") docs --path ~/projects/repo-docs

Use Cases:
  - Documentation branches (clean history)
  - GitHub Pages (isolated static site)
  - Configuration management (separate from code)
  - Multi-project mono-repo (isolated projects)
  - Localization branches (i18n)
  - API documentation (generated docs)

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
    --file)
      INITIAL_FILE="$2"
      shift 2
      ;;
    --content)
      INITIAL_CONTENT="$2"
      shift 2
      ;;
    --message)
      COMMIT_MESSAGE="$2"
      shift 2
      ;;
    --open)
      OPEN_IDE=1
      shift
      ;;
    --ide)
      IDE="$2"
      shift 2
      ;;
    --push)
      PUSH_REMOTE=1
      shift
      ;;
    --force-overwrite-branch)
      FORCE_OVERWRITE=1
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

# Set default content if not provided
if [[ -z "$INITIAL_CONTENT" ]]; then
  INITIAL_CONTENT="# $BRANCH"
fi

# Determine worktree path
if [[ -n "$CUSTOM_PATH" ]]; then
  WORKTREE_PATH="$CUSTOM_PATH"
else
  WORKTREE_PATH=$(wth_generate_worktree_path "$BRANCH")
fi

wth_info "Creating orphan branch with worktree: $BRANCH"
wth_info "Worktree path: $WORKTREE_PATH"

# Check if branch already exists
if wth_branch_exists "$BRANCH"; then
  if [[ "$FORCE_OVERWRITE" -eq 0 ]]; then
    wth_error "Branch '$BRANCH' already exists. Use --force-overwrite-branch to overwrite (DANGEROUS)"
    exit 1
  else
    wth_info "WARNING: Overwriting existing branch '$BRANCH'"
    if [[ "$DRY_RUN" -eq 0 ]]; then
      sleep 3
    fi
  fi
fi

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

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "+ git worktree add --orphan \"$BRANCH\" \"$WORKTREE_PATH\""
  echo "+ cd \"$WORKTREE_PATH\""
  echo "+ echo \"$INITIAL_CONTENT\" > \"$INITIAL_FILE\""
  echo "+ git add \"$INITIAL_FILE\""
  echo "+ git commit -m \"$COMMIT_MESSAGE\""
  if [[ "$PUSH_REMOTE" -eq 1 ]]; then
    echo "+ git push -u origin \"$BRANCH\""
  fi
else
  # Create orphan branch with worktree
  wth_info "Creating orphan branch and worktree..."
  
  # Delete existing branch if force overwrite
  if [[ "$FORCE_OVERWRITE" -eq 1 ]] && wth_branch_exists "$BRANCH"; then
    git branch -D "$BRANCH"
  fi
  
  # Create worktree with orphan branch
  git worktree add --orphan "$BRANCH" "$WORKTREE_PATH"
  
  # Initialize with content
  wth_info "Initializing with content..."
  (
    cd "$WORKTREE_PATH"
    echo "$INITIAL_CONTENT" > "$INITIAL_FILE"
    git add "$INITIAL_FILE"
    git commit -m "$COMMIT_MESSAGE"
  )
  
  wth_info "Orphan branch and worktree created successfully!"
  wth_info "Path: $WORKTREE_PATH"
  wth_info "Branch: $BRANCH"
  wth_info "Initial file: $INITIAL_FILE"
  
  # Push to remote if requested
  if [[ "$PUSH_REMOTE" -eq 1 ]]; then
    wth_info "Pushing to remote..."
    (cd "$WORKTREE_PATH" && git push -u origin "$BRANCH")
    wth_info "Pushed to origin/$BRANCH"
  fi
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
