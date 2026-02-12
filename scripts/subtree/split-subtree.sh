#!/usr/bin/env bash
#
# split-subtree.sh - Split a subtree into a new branch
#
# Usage:
#   split-subtree.sh --prefix <path> --branch <branch> [options]
#
# Examples:
#   split-subtree.sh --prefix lib/mylib --branch mylib-split
#   split-subtree.sh --prefix vendor/tool --branch tool-extracted
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/subtree-helpers.sh"

# Default values
PREFIX=""
NEW_BRANCH=""
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") --prefix <path> --branch <branch> [options]

Split a subtree into a new branch.

Required Options:
  --prefix <path>     Subtree prefix path
  --branch <branch>   New branch name for split history

Options:
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  # Split subtree to new branch
  $(basename "$0") --prefix lib/mylib --branch mylib-split

  # Split and extract
  $(basename "$0") --prefix vendor/tool --branch tool-extracted

Use Case:
  Extract a subtree's history into a separate branch, which can then
  be pushed to a new repository or used for other purposes.

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --prefix)
      PREFIX="$2"
      shift 2
      ;;
    --branch)
      NEW_BRANCH="$2"
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

# Validate required arguments
if [[ -z "$PREFIX" ]]; then
  sth_error "Prefix is required"
  usage
  exit 1
fi

if [[ -z "$NEW_BRANCH" ]]; then
  sth_error "Branch name is required"
  usage
  exit 1
fi

# Check if we're in a git repository
if ! git rev-parse --git-dir &>/dev/null; then
  sth_error "Not in a git repository"
  exit 1
fi

# Validate prefix
if ! sth_validate_prefix "$PREFIX"; then
  exit 1
fi

# Check if subtree exists
if ! sth_subtree_exists "$PREFIX"; then
  sth_error "Subtree does not exist at prefix: $PREFIX"
  exit 1
fi

# Check if branch already exists
if git show-ref --verify --quiet "refs/heads/$NEW_BRANCH"; then
  sth_error "Branch already exists: $NEW_BRANCH"
  exit 1
fi

sth_info "Splitting subtree:"
sth_info "  Prefix: $PREFIX"
sth_info "  New branch: $NEW_BRANCH"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "+ git subtree split --prefix \"$PREFIX\" --branch \"$NEW_BRANCH\""
else
  # Split subtree
  sth_info "Splitting subtree (this may take a while)..."
  
  git subtree split --prefix "$PREFIX" --branch "$NEW_BRANCH"
  
  sth_info "Subtree split successfully!"
  sth_info "New branch created: $NEW_BRANCH"
  sth_info ""
  sth_info "Next steps:"
  sth_info "  1. Review the split branch: git checkout $NEW_BRANCH"
  sth_info "  2. Push to new repository: git push <new-remote-url> $NEW_BRANCH:main"
fi

sth_info "Done!"
