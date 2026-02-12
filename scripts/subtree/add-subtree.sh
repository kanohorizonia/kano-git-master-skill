#!/usr/bin/env bash
#
# add-subtree.sh - Add a subtree from another repository
#
# Usage:
#   add-subtree.sh --prefix <path> --url <url> --branch <branch> [options]
#
# Examples:
#   add-subtree.sh --prefix lib/mylib --url https://github.com/user/mylib.git --branch main
#   add-subtree.sh --prefix vendor/tool --url git@github.com:org/tool.git --branch develop --squash
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
Usage: $(basename "$0") --prefix <path> --url <url> --branch <branch> [options]

Add a subtree from another repository.

Required Options:
  --prefix <path>     Subtree prefix path (e.g., lib/mylib)
  --url <url>         Remote repository URL

Options:
  --branch <branch>   Branch name (default: main)
  --squash            Squash commits into one
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  # Add subtree
  $(basename "$0") --prefix lib/mylib \\
    --url https://github.com/user/mylib.git \\
    --branch main

  # Add with squash
  $(basename "$0") --prefix vendor/tool \\
    --url git@github.com:org/tool.git \\
    --branch develop --squash

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

# Check if subtree already exists
if sth_subtree_exists "$PREFIX"; then
  sth_error "Subtree already exists at prefix: $PREFIX"
  exit 1
fi

# Check if directory already exists
if [[ -e "$PREFIX" ]]; then
  sth_error "Directory already exists: $PREFIX"
  exit 1
fi

sth_info "Adding subtree:"
sth_info "  Prefix: $PREFIX"
sth_info "  URL: $URL"
sth_info "  Branch: $BRANCH"
sth_info "  Squash: $([ $SQUASH -eq 1 ] && echo 'yes' || echo 'no')"

if [[ "$DRY_RUN" -eq 1 ]]; then
  if [[ "$SQUASH" -eq 1 ]]; then
    echo "+ git subtree add --prefix \"$PREFIX\" \"$URL\" \"$BRANCH\" --squash"
  else
    echo "+ git subtree add --prefix \"$PREFIX\" \"$URL\" \"$BRANCH\""
  fi
else
  # Add subtree
  sth_info "Adding subtree (this may take a while)..."
  
  if [[ "$SQUASH" -eq 1 ]]; then
    git subtree add --prefix "$PREFIX" "$URL" "$BRANCH" --squash
  else
    git subtree add --prefix "$PREFIX" "$URL" "$BRANCH"
  fi
  
  sth_info "Subtree added successfully!"
  sth_info "Path: $PREFIX"
fi

sth_info "Done!"
