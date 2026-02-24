#!/usr/bin/env bash
#
# create-orphan-branch.sh - Create and initialize orphan branches
#
# Purpose:
#   Create orphan branches (branches with no parent commits) with proper
#   safety checks and stash management. Useful for creating isolated branches
#   for development tooling, documentation, or other purposes.
#
# Usage:
#   ./create-orphan-branch.sh --branch <name> [options]
#
# Arguments:
#   --branch <name>         Branch name (required)
#
# Options:
#   --file <name>           Initial file name (default: README.md)
#   --content <text>        File content (default: "# {branch}")
#   --message <text>        Commit message (default: "Initial commit")
#   --push                  Push to remote after creation
#   --return                Return to original branch after creation
#   --force-overwrite-branch  Overwrite existing branch (DANGEROUS)
#   --dir <path>            Repository directory (default: current directory)
#   --dry-run               Show what would be done without making changes
#   -h, --help              Show help
#
# Safety Features:
#   - Pre-checks if branch exists locally or remotely
#   - Auto-stashes uncommitted changes before switching branches
#   - Restores stash when returning to original branch
#   - Verbose flag name (--force-overwrite-branch) to prevent accidents
#   - 3-second warning delay before destructive operations
#
# Examples:
#   # Create orphan branch
#   ./create-orphan-branch.sh --branch dev/tools
#
#   # Create and push to remote
#   ./create-orphan-branch.sh --branch dev/tools --push
#
#   # Create, then return to original branch
#   ./create-orphan-branch.sh --branch dev/tools --return
#
#   # Custom initial file
#   ./create-orphan-branch.sh --branch dev/tools \
#     --file .gitignore \
#     --content "node_modules/"
#
#   # Force overwrite existing branch (DANGEROUS!)
#   ./create-orphan-branch.sh --branch dev/tools --force-overwrite-branch
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"

# Source git-helpers
if [[ -f "$LIB_DIR/git-helpers.sh" ]]; then
  source "$LIB_DIR/git-helpers.sh"
else
  echo "ERROR: Cannot find git-helpers.sh at $LIB_DIR/git-helpers.sh" >&2
  exit 1
fi

# Default configuration
BRANCH_NAME=""
FILE_NAME="README.md"
FILE_CONTENT=""
COMMIT_MESSAGE="Initial commit"
PUSH_TO_REMOTE=0
RETURN_TO_ORIGINAL=0
FORCE_OVERWRITE=0
REPO_DIR="."
DRY_RUN=0

# State tracking
ORIGINAL_BRANCH=""
STASH_REF=""

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") --branch <name> [options]

Create and initialize orphan branches with proper safety checks.

Arguments:
  --branch <name>         Branch name (required)

Options:
  --file <name>           Initial file name (default: README.md)
  --content <text>        File content (default: "# {branch}")
  --message <text>        Commit message (default: "Initial commit")
  --push                  Push to remote after creation
  --return                Return to original branch after creation
  --force-overwrite-branch  Overwrite existing branch (DANGEROUS)
  --dir <path>            Repository directory (default: current directory)
  --dry-run               Show what would be done without making changes
  -h, --help              Show help

Examples:
  # Create orphan branch
  ./create-orphan-branch.sh --branch dev/tools

  # Create and push to remote
  ./create-orphan-branch.sh --branch dev/tools --push

  # Create, then return to original branch
  ./create-orphan-branch.sh --branch dev/tools --return

  # Custom initial file
  ./create-orphan-branch.sh --branch dev/tools \\
    --file .gitignore \\
    --content "node_modules/"

  # Force overwrite existing branch (DANGEROUS!)
  ./create-orphan-branch.sh --branch dev/tools --force-overwrite-branch

Safety:
  - Script checks if branch exists before creating
  - Uncommitted changes are automatically stashed
  - Stash is restored when returning to original branch
  - Force flag requires verbose name to prevent accidents
EOF
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Validate prerequisites
  gith_validate_prerequisites --require-git --require-git-repo --script-name "$(basename "$0")"

  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --branch)
        BRANCH_NAME="${2:-}"
        shift 2
        ;;
      --file)
        FILE_NAME="${2:-}"
        shift 2
        ;;
      --content)
        FILE_CONTENT="${2:-}"
        shift 2
        ;;
      --message)
        COMMIT_MESSAGE="${2:-}"
        shift 2
        ;;
      --push)
        PUSH_TO_REMOTE=1
        shift
        ;;
      --return)
        RETURN_TO_ORIGINAL=1
        shift
        ;;
      --force-overwrite-branch)
        FORCE_OVERWRITE=1
        shift
        ;;
      --dir)
        REPO_DIR="${2:-}"
        shift 2
        ;;
      --dry-run)
        DRY_RUN=1
        export DRY_RUN
        shift
        ;;
      -*)
        gith_error "Unknown option: $1"
        usage
        exit 1
        ;;
      *)
        gith_error "Unexpected argument: $1"
        usage
        exit 1
        ;;
    esac
  done

  # Validate required arguments
  if [[ -z "$BRANCH_NAME" ]]; then
    gith_error "Branch name is required (--branch)"
    usage
    exit 1
  fi

  # Set default content if not specified
  if [[ -z "$FILE_CONTENT" ]]; then
    FILE_CONTENT="# $BRANCH_NAME"
  fi

  # Validate repository directory
  if [[ ! -d "$REPO_DIR" ]]; then
    gith_error "Directory does not exist: $REPO_DIR"
    exit 1
  fi

  # Check if it's a git repository
  if ! gith_is_git_repo "$REPO_DIR"; then
    gith_error "Not a git repository: $REPO_DIR"
    exit 1
  fi

  # Show configuration
  gith_log "INFO" "Creating orphan branch..."
  gith_log "INFO" "  Branch: $BRANCH_NAME"
  gith_log "INFO" "  File: $FILE_NAME"
  gith_log "INFO" "  Repository: $REPO_DIR"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    gith_log "INFO" "  Mode: DRY-RUN (no changes will be made)"
  fi

  # Validate branch name
  if ! gith_validate_branch_name "$BRANCH_NAME"; then
    gith_error "Invalid branch name: $BRANCH_NAME"
    exit 1
  fi

  # Check if branch exists locally
  local branch_exists_local=0
  if (cd "$REPO_DIR" && git show-ref --verify --quiet "refs/heads/$BRANCH_NAME" 2>/dev/null); then
    branch_exists_local=1
    gith_log "DEBUG" "Branch exists locally: $BRANCH_NAME"
  fi

  # Check if branch exists remotely
  local branch_exists_remote=0
  if (cd "$REPO_DIR" && git ls-remote --heads origin "$BRANCH_NAME" 2>/dev/null | grep -q "$BRANCH_NAME"); then
    branch_exists_remote=1
    gith_log "DEBUG" "Branch exists remotely: $BRANCH_NAME"
  fi

  # Handle existing branch
  if [[ $branch_exists_local -eq 1 ]] || [[ $branch_exists_remote -eq 1 ]]; then
    if [[ "$FORCE_OVERWRITE" -eq 0 ]]; then
      gith_error "Branch already exists: $BRANCH_NAME"
      if [[ $branch_exists_local -eq 1 ]]; then
        gith_error "  - Exists locally"
      fi
      if [[ $branch_exists_remote -eq 1 ]]; then
        gith_error "  - Exists remotely"
      fi
      gith_error ""
      gith_error "If you really want to overwrite the branch, use:"
      gith_error "  --force-overwrite-branch"
      gith_error ""
      gith_error "WARNING: This will DESTROY the existing branch!"
      exit 1
    else
      # Force flag is set - show warning and wait
      gith_log "WARN" "⚠️  WARNING: --force-overwrite-branch flag detected"
      gith_log "WARN" "⚠️  This will DESTROY the existing branch: $BRANCH_NAME"
      if [[ $branch_exists_local -eq 1 ]]; then
        gith_log "WARN" "⚠️  - Local branch will be deleted"
      fi
      if [[ $branch_exists_remote -eq 1 ]]; then
        gith_log "WARN" "⚠️  - Remote branch will be overwritten"
      fi

      if [[ "$DRY_RUN" -eq 0 ]]; then
        gith_log "WARN" "⚠️  Proceeding in 3 seconds... (Ctrl+C to cancel)"
        sleep 3
      fi

      # Delete local branch if it exists
      if [[ $branch_exists_local -eq 1 ]]; then
        if [[ "$DRY_RUN" -eq 0 ]]; then
          gith_log "INFO" "Deleting local branch: $BRANCH_NAME"
          (cd "$REPO_DIR" && git branch -D "$BRANCH_NAME" 2>/dev/null) || true
        else
          gith_log "INFO" "[DRY-RUN] Would delete local branch: $BRANCH_NAME"
        fi
      fi
    fi
  fi

  # Get current branch for potential return
  ORIGINAL_BRANCH=$(gith_get_current_branch "$REPO_DIR")
  if [[ -z "$ORIGINAL_BRANCH" ]]; then
    gith_log "WARN" "Currently in detached HEAD state"
    if [[ "$RETURN_TO_ORIGINAL" -eq 1 ]]; then
      gith_error "Cannot use --return when in detached HEAD state"
      exit 1
    fi
  else
    gith_log "INFO" "Current branch: $ORIGINAL_BRANCH"
  fi

  # Stash uncommitted changes if any
  if gith_has_changes "$REPO_DIR"; then
    gith_log "INFO" "Uncommitted changes detected, creating stash..."
    STASH_REF=$(gith_stash_create "$REPO_DIR" "auto-stash before creating orphan branch $BRANCH_NAME")
    if [[ $? -ne 0 ]]; then
      gith_error "Failed to create stash"
      exit 1
    fi
    gith_log "INFO" "Stash created: $STASH_REF"
  else
    gith_log "INFO" "No uncommitted changes to stash"
  fi

  # Create orphan branch
  gith_log "INFO" "Creating orphan branch: $BRANCH_NAME"
  if [[ "$DRY_RUN" -eq 0 ]]; then
    if ! (cd "$REPO_DIR" && git checkout --orphan "$BRANCH_NAME" 2>&1); then
      gith_error "Failed to create orphan branch"
      # Restore stash if we created one
      if [[ -n "$STASH_REF" ]]; then
        gith_log "INFO" "Restoring stash..."
        gith_stash_pop "$REPO_DIR" "$STASH_REF"
      fi
      exit 1
    fi
  else
    gith_log "INFO" "[DRY-RUN] Would create orphan branch: $BRANCH_NAME"
  fi

  # Remove all files from index
  gith_log "INFO" "Clearing index..."
  if [[ "$DRY_RUN" -eq 0 ]]; then
    (cd "$REPO_DIR" && git rm -rf . 2>/dev/null) || true
  else
    gith_log "INFO" "[DRY-RUN] Would clear index"
  fi

  # Create initial file
  gith_log "INFO" "Creating initial file: $FILE_NAME"
  if [[ "$DRY_RUN" -eq 0 ]]; then
    echo "$FILE_CONTENT" > "$REPO_DIR/$FILE_NAME"
    (cd "$REPO_DIR" && git add "$FILE_NAME")
  else
    gith_log "INFO" "[DRY-RUN] Would create file: $FILE_NAME"
  fi

  # Create initial commit
  gith_log "INFO" "Creating initial commit..."
  if [[ "$DRY_RUN" -eq 0 ]]; then
    if ! (cd "$REPO_DIR" && git commit -m "$COMMIT_MESSAGE" 2>&1); then
      gith_error "Failed to create initial commit"
      # Try to return to original branch
      if [[ -n "$ORIGINAL_BRANCH" ]]; then
        gith_log "INFO" "Returning to original branch: $ORIGINAL_BRANCH"
        (cd "$REPO_DIR" && git checkout "$ORIGINAL_BRANCH" 2>/dev/null) || true
        if [[ -n "$STASH_REF" ]]; then
          gith_stash_pop "$REPO_DIR" "$STASH_REF"
        fi
      fi
      exit 1
    fi

    local commit_hash
    commit_hash=$(cd "$REPO_DIR" && git rev-parse --short HEAD)
    gith_log "INFO" "Initial commit created: $commit_hash"
  else
    gith_log "INFO" "[DRY-RUN] Would create initial commit: $COMMIT_MESSAGE"
  fi

  # Optional: Push to remote
  if [[ "$PUSH_TO_REMOTE" -eq 1 ]]; then
    gith_log "INFO" "Pushing to remote..."
    if [[ "$DRY_RUN" -eq 0 ]]; then
      if [[ "$FORCE_OVERWRITE" -eq 1 ]]; then
        # Force push if overwriting existing branch
        if ! (cd "$REPO_DIR" && git push -f origin "$BRANCH_NAME" 2>&1); then
          gith_error "Failed to push to remote"
          gith_error "Branch created locally but not pushed"
          exit 1
        fi
      else
        # Normal push
        if ! (cd "$REPO_DIR" && git push -u origin "$BRANCH_NAME" 2>&1); then
          gith_error "Failed to push to remote"
          gith_error "Branch created locally but not pushed"
          exit 1
        fi
      fi
      gith_log "INFO" "Pushed to remote successfully"
    else
      gith_log "INFO" "[DRY-RUN] Would push to remote: origin/$BRANCH_NAME"
    fi
  fi

  # Optional: Return to original branch
  if [[ "$RETURN_TO_ORIGINAL" -eq 1 ]]; then
    if [[ -z "$ORIGINAL_BRANCH" ]]; then
      gith_log "WARN" "Cannot return to original branch (was in detached HEAD)"
    else
      gith_log "INFO" "Returning to original branch: $ORIGINAL_BRANCH"
      if [[ "$DRY_RUN" -eq 0 ]]; then
        if ! (cd "$REPO_DIR" && git checkout "$ORIGINAL_BRANCH" 2>&1); then
          gith_error "Failed to return to original branch"
          gith_error "You are still on branch: $BRANCH_NAME"
          exit 1
        fi

        # Restore stash if we created one
        if [[ -n "$STASH_REF" ]]; then
          gith_log "INFO" "Restoring stash: $STASH_REF"
          if ! gith_stash_pop "$REPO_DIR" "$STASH_REF"; then
            gith_error "Failed to restore stash"
            gith_error "Stash still exists: $STASH_REF"
            gith_error "You can manually restore it with: git stash apply $STASH_REF"
            exit 1
          fi
          gith_log "INFO" "Stash restored successfully"
        fi
      else
        gith_log "INFO" "[DRY-RUN] Would return to branch: $ORIGINAL_BRANCH"
        if [[ -n "$STASH_REF" ]]; then
          gith_log "INFO" "[DRY-RUN] Would restore stash: $STASH_REF"
        fi
      fi
    fi
  fi

  gith_log "INFO" "Orphan branch creation complete"
}

# Run main
main "$@"
