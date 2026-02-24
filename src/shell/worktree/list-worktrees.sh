#!/usr/bin/env bash
#
# list-worktrees.sh - List all worktrees with metadata
#
# Usage:
#   list-worktrees.sh [options]
#
# Examples:
#   list-worktrees.sh
#   list-worktrees.sh --format json
#   list-worktrees.sh --detailed
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/worktree-helpers.sh"

# Default values
FORMAT="table"
DETAILED=0

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

List all worktrees with metadata.

Options:
  --format <format>   Output format: table, json (default: table)
  --detailed          Show detailed information
  -h, --help          Show this help

Examples:
  # List all worktrees
  $(basename "$0")

  # JSON output
  $(basename "$0") --format json

  # Show detailed info
  $(basename "$0") --detailed

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --format)
      FORMAT="$2"
      shift 2
      ;;
    --detailed)
      DETAILED=1
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

# Output based on format
case "$FORMAT" in
  json)
    wth_list_worktrees_json
    ;;
  table)
    # Table header
    if [[ "$DETAILED" -eq 1 ]]; then
      printf "%-40s %-20s %-8s %-12s %s\n" "Worktree" "Branch" "Orphan" "Status" "Last Commit"
      printf "%-40s %-20s %-8s %-12s %s\n" "========" "======" "======" "======" "==========="
    else
      printf "%-40s %-20s %-8s %-12s\n" "Worktree" "Branch" "Orphan" "Status"
      printf "%-40s %-20s %-8s %-12s\n" "========" "======" "======" "======"
    fi
    
    # Parse worktree list
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
        local is_orphan="No"
        if [[ -n "$branch" ]] && wth_is_orphan_branch "$branch"; then
          is_orphan="Yes"
        fi
        
        local status
        status=$(wth_get_status "$path")
        
        if [[ "$DETAILED" -eq 1 ]]; then
          local last_commit
          last_commit=$(wth_get_last_commit "$path")
          printf "%-40s %-20s %-8s %-12s %s\n" "$path" "${branch:-detached}" "$is_orphan" "$status" "$last_commit"
        else
          printf "%-40s %-20s %-8s %-12s\n" "$path" "${branch:-detached}" "$is_orphan" "$status"
        fi
        
        path=""
        branch=""
        commit=""
      fi
    done < <(git worktree list --porcelain)
    ;;
  *)
    wth_error "Unknown format: $FORMAT"
    exit 1
    ;;
esac
