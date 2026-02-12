#!/usr/bin/env bash
#
# update-repo.sh - Update a single repository and all its submodules
#
# Purpose:
#   Quickly update a repository and all its submodules to the latest version.
#   This is a simple, focused script for the most common update workflow.
#
# Usage:
#   ./update-repo.sh [path] [options]
#
# Arguments:
#   path              Repository path (default: current directory)
#
# Options:
#   --remote <name>   Remote name (default: origin)
#   --no-stash        Fail if there are local changes
#   --dry-run         Show what would be done
#   -h, --help        Show help
#
# Examples:
#   # Update current directory
#   cd /path/to/my-repo
#   ./update-repo.sh
#
#   # Update specific repository
#   ./update-repo.sh /path/to/my-repo
#
#   # Dry-run mode
#   ./update-repo.sh --dry-run
#
#   # Specify remote (default: origin)
#   ./update-repo.sh --remote upstream
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

REPO_PATH="."
REMOTE_NAME="origin"
NO_STASH=0
DRY_RUN=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") [path] [options]

Update a repository and all its submodules to the latest version.

Arguments:
  path              Repository path (default: current directory)

Options:
  --remote <name>   Remote name (default: origin)
  --no-stash        Fail if there are local changes
  --dry-run         Show what would be done
  -h, --help        Show help

Examples:
  # Update current directory
  cd /path/to/my-repo
  ./update-repo.sh

  # Update specific repository
  ./update-repo.sh /path/to/my-repo

  # Dry-run mode
  ./update-repo.sh --dry-run

  # Specify remote
  ./update-repo.sh --remote upstream

Works with any Git remote provider (GitHub, GitLab, Azure Repos, Bitbucket, self-hosted, etc.)
EOF
}

update_repository() {
  local repo_path="$1"
  local remote_name="$2"
  local stash_ref=""
  local stash_created=0
  
  gith_log "INFO" "Updating repository: $repo_path"
  
  # Change to repository directory
  cd "$repo_path"
  
  # Check for uncommitted changes
  if gith_has_changes "$repo_path"; then
    if [[ "$NO_STASH" -eq 1 ]]; then
      gith_error "Repository has uncommitted changes and --no-stash is set"
      gith_error "Please commit or stash your changes before updating"
      return 1
    fi
    
    # Create stash
    stash_ref="$(gith_stash_create "$repo_path" "auto-stash-update-repo")"
    if [[ $? -eq 0 ]] && [[ -n "$stash_ref" ]]; then
      stash_created=1
      gith_log "INFO" "Created stash: $stash_ref"
    fi
  fi
  
  # Fetch from remote
  if ! gith_fetch_remote "$remote_name" "$repo_path"; then
    gith_error "Failed to fetch from remote: $remote_name"
    if [[ $stash_created -eq 1 ]]; then
      gith_log "INFO" "Stash preserved: $stash_ref"
      gith_log "INFO" "To restore: git stash pop $stash_ref"
    fi
    return 1
  fi
  
  # Get current branch
  local current_branch
  current_branch="$(gith_get_current_branch "$repo_path")"
  
  if [[ -z "$current_branch" ]]; then
    gith_error "Repository is in detached HEAD state"
    gith_error "Please checkout a branch before updating"
    if [[ $stash_created -eq 1 ]]; then
      gith_log "INFO" "Stash preserved: $stash_ref"
      gith_log "INFO" "To restore: git stash pop $stash_ref"
    fi
    return 1
  fi
  
  gith_log "INFO" "Current branch: $current_branch"
  
  # Check if current branch exists on remote
  local target_branch="$current_branch"
  if ! gith_branch_exists_on_remote "$remote_name" "$current_branch" "$repo_path"; then
    gith_log "WARN" "Branch '$current_branch' does not exist on remote '$remote_name'"
    
    # Try to detect default branch
    local default_branch
    default_branch="$(gith_get_default_branch "$remote_name" "$repo_path")"
    
    if [[ -z "$default_branch" ]]; then
      gith_error "Could not detect default branch for remote: $remote_name"
      if [[ $stash_created -eq 1 ]]; then
        gith_log "INFO" "Stash preserved: $stash_ref"
        gith_log "INFO" "To restore: git stash pop $stash_ref"
      fi
      return 1
    fi
    
    target_branch="$default_branch"
    gith_log "INFO" "Using default branch: $target_branch"
  fi
  
  # Rebase onto remote branch
  gith_log "INFO" "Rebasing onto: $remote_name/$target_branch"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    gith_log "INFO" "[DRY-RUN] Would rebase onto: $remote_name/$target_branch"
  else
    if ! git rebase "$remote_name/$target_branch" 2>&1; then
      gith_error "Rebase failed"
      gith_error "Please resolve conflicts manually"
      if [[ $stash_created -eq 1 ]]; then
        gith_log "INFO" "Stash preserved: $stash_ref"
        gith_log "INFO" "After resolving conflicts:"
        gith_log "INFO" "  git rebase --continue"
        gith_log "INFO" "  git stash pop $stash_ref"
      fi
      return 1
    fi
    gith_log "INFO" "Rebase completed successfully"
  fi
  
  # Update submodules
  if [[ -f "$repo_path/.gitmodules" ]]; then
    gith_log "INFO" "Updating submodules..."
    
    if [[ "$DRY_RUN" -eq 1 ]]; then
      gith_log "INFO" "[DRY-RUN] Would update submodules recursively"
    else
      if git submodule update --init --recursive --remote 2>&1 | while IFS= read -r line; do
        if [[ "$line" =~ ^Submodule\ path\ \'([^\']+)\':\ checked\ out\ \'([^\']+)\' ]]; then
          local submodule_path="${BASH_REMATCH[1]}"
          local commit_hash="${BASH_REMATCH[2]}"
          gith_log "INFO" "Submodule '$submodule_path': checked out '$commit_hash'"
        else
          echo "$line"
        fi
      done; then
        gith_log "INFO" "Submodules updated successfully"
      else
        gith_error "Failed to update submodules"
        if [[ $stash_created -eq 1 ]]; then
          gith_log "INFO" "Stash preserved: $stash_ref"
          gith_log "INFO" "To restore: git stash pop $stash_ref"
        fi
        return 1
      fi
    fi
  else
    gith_log "INFO" "No submodules found"
  fi
  
  # Pop stash if created
  if [[ $stash_created -eq 1 ]]; then
    if ! gith_stash_pop "$repo_path" "$stash_ref"; then
      gith_error "Failed to restore stash"
      gith_error "Stash preserved: $stash_ref"
      gith_error "To restore manually: git stash pop $stash_ref"
      return 1
    fi
    gith_log "INFO" "Stash restored successfully"
  fi
  
  gith_log "INFO" "Update complete!"
  return 0
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --remote)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --remote requires an argument"
          usage
          exit 1
        fi
        REMOTE_NAME="$2"
        shift 2
        ;;
      --no-stash)
        NO_STASH=1
        shift
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
        # First positional argument is the repo path
        if [[ "$REPO_PATH" == "." ]]; then
          REPO_PATH="$1"
        else
          gith_error "Too many positional arguments"
          usage
          exit 1
        fi
        shift
        ;;
    esac
  done
  
  # Validate repository path
  if [[ ! -d "$REPO_PATH" ]]; then
    gith_error "Directory does not exist: $REPO_PATH"
    exit 1
  fi
  
  # Check if it's a git repository
  if ! gith_is_git_repo "$REPO_PATH"; then
    gith_error "Not a git repository: $REPO_PATH"
    exit 1
  fi
  
  # Check if remote exists
  if ! gith_has_remote "$REMOTE_NAME" "$REPO_PATH"; then
    gith_error "Remote does not exist: $REMOTE_NAME"
    gith_error "Available remotes:"
    (cd "$REPO_PATH" && git remote -v)
    exit 1
  fi
  
  # Update repository
  if update_repository "$REPO_PATH" "$REMOTE_NAME"; then
    exit 0
  else
    exit 1
  fi
}

# Run main function
main "$@"
