#!/usr/bin/env bash
#
# list-subtrees.sh - List all subtrees in repository
#
# Usage:
#   list-subtrees.sh [options]
#
# Examples:
#   list-subtrees.sh
#   list-subtrees.sh --format json
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/subtree-helpers.sh"

# Default values
FORMAT="table"

usage() {
  cat << EOF
Usage: $(basename "$0") [options]

List all subtrees in repository.

Options:
  --format <format>   Output format: table, json (default: table)
  -h, --help          Show this help

Examples:
  # List all subtrees
  $(basename "$0")

  # JSON output
  $(basename "$0") --format json

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
  sth_error "Not in a git repository"
  exit 1
fi

# Get all subtrees
SUBTREES=$(sth_detect_subtrees)

if [[ -z "$SUBTREES" ]]; then
  sth_info "No subtrees found in this repository"
  exit 0
fi

# Output based on format
case "$FORMAT" in
  json)
    sth_list_subtrees_json
    ;;
  table)
    # Table header
    printf "%-40s %-50s %-20s\n" "Prefix" "Remote" "Last Sync"
    printf "%-40s %-50s %-20s\n" "======" "======" "========="
    
    # List each subtree
    while IFS= read -r prefix; do
      remote=$(sth_get_subtree_remote "$prefix")
      last_sync=$(sth_get_last_sync_date "$prefix")
      
      # Truncate remote if too long
      if [[ ${#remote} -gt 50 ]]; then
        remote="${remote:0:47}..."
      fi
      
      printf "%-40s %-50s %-20s\n" "$prefix" "${remote:-unknown}" "$last_sync"
    done <<< "$SUBTREES"
    ;;
  *)
    sth_error "Unknown format: $FORMAT"
    exit 1
    ;;
esac
