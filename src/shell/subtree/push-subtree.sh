#!/usr/bin/env bash
#
# push-subtree.sh - Push changes from a subtree back to its source repository
#
# Usage:
#   push-subtree.sh --prefix <path> --url <url> [options]
#
# Examples:
#   push-subtree.sh --prefix lib/mylib --url https://github.com/user/mylib.git
#   push-subtree.sh --prefix vendor/tool --url git@github.com:org/tool.git --branch develop
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/subtree-helpers.sh"

# Default values
PREFIX=""
URL=""
BRANCH="main"
DRY_RUN=0

usage() {
  cat << EOF
Usage: $(basename "$0") --prefix <path> --url <url> [options]

Push changes from a subtree back to its source repository.

Required Options:
  --prefix <path>     Subtree prefix path
  --url <url>         Remote repository URL

Options:
  --branch <branch>   Branch name (default: main)
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  # Push changes
  $(basename "$0") --prefix lib/mylib \\
    --url https://github.com/user/mylib.git

  # Push to specific branch
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

if [[ -z "$URL" ]]; then
  sth_error "URL is required"
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

# Validate URL
if ! sth_validate_url "$URL"; then
  exit 1
fi

# Check if subtree exists
if ! sth_subtree_exists "$PREFIX"; then
  sth_error "Subtree does not exist at prefix: $PREFIX"
  exit 1
fi

sth_info "Pushing subtree changes:"
sth_info "  Prefix: $PREFIX"
sth_info "  URL: $URL"
sth_info "  Branch: $BRANCH"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "+ git subtree push --prefix \"$PREFIX\" \"$URL\" \"$BRANCH\""
else
  # Push subtree changes
  sth_info "Pushing changes (this may take a while)..."
  
  git subtree push --prefix "$PREFIX" "$URL" "$BRANCH"
  
  sth_info "Subtree changes pushed successfully!"
fi

sth_info "Done!"
