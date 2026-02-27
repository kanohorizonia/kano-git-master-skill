#!/usr/bin/env bash
#
# sync-worktrees.sh - Sync all worktrees (fetch, pull, status)
#
# Usage:
#   sync-worktrees.sh [options]
#
# Examples:
#   sync-worktrees.sh
#   sync-worktrees.sh --status
#   sync-worktrees.sh --worktrees "main,docs"
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/worktree-helpers.sh"

# Default values
SHOW_STATUS=0
FILTER_WORKTREES=""
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

Sync all worktrees (fetch, pull, status).

Options:
  --status                Show status after sync
  --worktrees <list>      Comma-separated list of branches to sync (default: all)
  --dry-run               Show what would be done
  -h, --help              Show this help

Examples:
  # Sync all worktrees
  $(basename "$0")

  # Sync and show status
  $(basename "$0") --status

  # Sync specific worktrees
  $(basename "$0") --worktrees "main,docs"

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --status)
      SHOW_STATUS=1
      shift
      ;;
    --worktrees)
      FILTER_WORKTREES="$2"
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
      echo "Error: Unexpected argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

# Check if we're in a git repository
if ! git rev-parse --git-dir &>/dev/null; then
  wth_error "Not in a git repository"
  exit 1
fi

# Convert filter to array
IFS=',' read -ra FILTER_ARRAY <<< "$FILTER_WORKTREES"

# Sync each worktree
wth_info "Syncing worktrees..."

path=""
branch=""
commit=""
synced_count=0
failed_count=0

while IFS= read -r line; do
  if [[ "$line" =~ ^worktree\ (.+)$ ]]; then
    path="${BASH_REMATCH[1]}"
  elif [[ "$line" =~ ^branch\ refs/heads/(.+)$ ]]; then
    branch="${BASH_REMATCH[1]}"
  elif [[ "$line" =~ ^HEAD\ ([a-f0-9]+)$ ]]; then
    commit="${BASH_REMATCH[1]}"
  elif [[ -z "$line" && -n "$path" ]]; then
    # End of worktree entry
    
    # Check if this worktree should be synced
    should_sync=1
    if [[ -n "$FILTER_WORKTREES" ]]; then
      should_sync=0
      for filter_branch in "${FILTER_ARRAY[@]}"; do
        if [[ "$branch" == "$filter_branch" ]]; then
          should_sync=1
          break
        fi
      done
    fi
    
    if [[ "$should_sync" -eq 1 && -n "$branch" ]]; then
      wth_info "Syncing: $path ($branch)"
      
      if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "+ cd \"$path\""
        echo "+ git fetch --all --prune"
        echo "+ git pull --rebase"
      else
        (
          cd "$path"
          
          # Fetch
          if git fetch --all --prune 2>&1; then
            wth_info "  Fetched successfully"
          else
            wth_error "  Failed to fetch"
            exit 1
          fi
          
          # Pull with rebase
          if git pull --rebase 2>&1; then
            wth_info "  Pulled successfully"
          else
            wth_error "  Failed to pull (may have conflicts)"
            exit 1
          fi
        )
        
        if [[ $? -eq 0 ]]; then
          ((synced_count++))
        else
          ((failed_count++))
        fi
      fi
    fi
    
    path=""
    branch=""
    commit=""
  fi
done < <(git worktree list --porcelain)

# Show summary
wth_info "Sync complete!"
if [[ "$DRY_RUN" -eq 0 ]]; then
  wth_info "Synced: $synced_count, Failed: $failed_count"
fi

# Show status if requested
if [[ "$SHOW_STATUS" -eq 1 && "$DRY_RUN" -eq 0 ]]; then
  wth_info ""
  wth_info "Worktree Status:"
  "$SCRIPT_DIR/list-worktrees.sh"
fi
