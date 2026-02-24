#!/usr/bin/env bash
#
# open-worktree.sh - Open worktree in IDE
#
# Usage:
#   open-worktree.sh <branch> [options]
#
# Examples:
#   open-worktree.sh docs
#   open-worktree.sh docs --ide idea
#   open-worktree.sh docs --terminal
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/worktree-helpers.sh"

# Default values
BRANCH=""
IDE="auto"
USE_TERMINAL=0

usage() {
  cat << EOF
Usage: $(basename "$0") <branch> [options]

Open worktree in IDE.

Arguments:
  branch              Branch name (required)

Options:
  --ide <name>        IDE to use: auto, code, idea, vim (default: auto)
  --terminal          Open in terminal instead of IDE
  -h, --help          Show this help

Examples:
  # Open in default IDE (VS Code)
  $(basename "$0") docs

  # Open in specific IDE
  $(basename "$0") docs --ide idea

  # Open in terminal
  $(basename "$0") docs --terminal

Supported IDEs:
  auto      - Auto-detect (VS Code > IntelliJ > terminal)
  code      - Visual Studio Code
  idea      - IntelliJ IDEA
  vim       - Vim/Neovim
  terminal  - Open in terminal

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --ide)
      IDE="$2"
      shift 2
      ;;
    --terminal)
      USE_TERMINAL=1
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

# Check if worktree exists
if ! wth_worktree_exists "$BRANCH"; then
  wth_error "Worktree does not exist for branch: $BRANCH"
  exit 1
fi

# Get worktree path
WORKTREE_PATH=$(wth_get_worktree_path "$BRANCH")

wth_info "Opening worktree: $WORKTREE_PATH"
wth_info "Branch: $BRANCH"

# Override IDE if terminal requested
if [[ "$USE_TERMINAL" -eq 1 ]]; then
  IDE="terminal"
fi

# Open in IDE
if wth_open_in_ide "$WORKTREE_PATH" "$IDE"; then
  wth_info "Opened successfully in: $IDE"
else
  wth_error "Failed to open in IDE: $IDE"
  exit 1
fi
