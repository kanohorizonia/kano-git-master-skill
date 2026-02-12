#!/usr/bin/env bash
#
# pull-subtree.sh - Pull updates from a subtree's source repository
#
# Usage:
#   pull-subtree.sh --prefix <path> [options]
#
# Examples:
#   pull-subtree.sh --prefix lib/mylib
#   pull-subtree.sh --prefix vendor/tool --squash
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/subtree-helpers.sh"

# Default values
PREFIX=""
URL=""
BRANCH="main"
SQUASH=0
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") --prefix <path> [options]

Pull updates from a subtree's source repository.

Required Options:
  --prefix <path>     Subtree prefix path

Options:
  --url <url>         Remote repository URL (auto-detected if not provided)
  --branch <branch>   Branch name (default: main)
  --squash            Squash commits into one
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  # Pull updates
  $(basename "$0") --prefix lib/mylib

  # Pull with squash
  $(basename "$0") --prefix lib/mylib --squash

  # Pull from specific URL and branch
  $(basename "$0") --prefix lib/mylib \\
    --url https://github.com/user/mylib.git \\
    --branch develop

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
    --url)
      URL="$2"
      shift 2
      ;;
    --branch)
      BRANCH="$2"
      shift 2
      ;;
    --squash)
      SQUASH=1
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
  sth_info "Use add-subtree.sh to add a new subtree"
  exit 1
fi

# Auto-detect URL if not provided
if [[ -z "$URL" ]]; then
  sth_info "Auto-detecting remote URL..."
  URL=$(sth_get_subtree_remote "$PREFIX")
  
  if [[ -z "$URL" ]]; then
    sth_error "Could not auto-detect remote URL"
    sth_info "Please provide --url option"
    exit 1
  fi
  
  sth_info "Detected URL: $URL"
fi

# Validate URL
if ! sth_validate_url "$URL"; then
  exit 1
fi

sth_info "Pulling subtree updates:"
sth_info "  Prefix: $PREFIX"
sth_info "  URL: $URL"
sth_info "  Branch: $BRANCH"
sth_info "  Squash: $([ $SQUASH -eq 1 ] && echo 'yes' || echo 'no')"

if [[ "$DRY_RUN" -eq 1 ]]; then
  if [[ "$SQUASH" -eq 1 ]]; then
    echo "+ git subtree pull --prefix \"$PREFIX\" \"$URL\" \"$BRANCH\" --squash"
  else
    echo "+ git subtree pull --prefix \"$PREFIX\" \"$URL\" \"$BRANCH\""
  fi
else
  # Pull subtree updates
  sth_info "Pulling updates (this may take a while)..."
  
  if [[ "$SQUASH" -eq 1 ]]; then
    git subtree pull --prefix "$PREFIX" "$URL" "$BRANCH" --squash
  else
    git subtree pull --prefix "$PREFIX" "$URL" "$BRANCH"
  fi
  
  sth_info "Subtree updated successfully!"
fi

sth_info "Done!"
