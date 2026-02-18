#!/usr/bin/env bash
#
# smart-sync-noai.sh - Standard git sync without AI
#
# Purpose:
#   Synchronize current branch with upstream using standard git rebase.
#   No AI providers - pure git operations.
#
# Usage:
#   ./smart-sync-noai.sh [options]
#
# Options:
#   --onto <branch>             Sync onto branch (default: upstream)
#   --interactive               Interactive rebase
#   --auto-squash               Auto-squash fixup commits
#   --strategy <name>           Rebase strategy (merge, ours, theirs)
#   --dry-run                   Show what would be done
#   -h, --help                  Show help
#
# Examples:
#   ./smart-sync-noai.sh
#   ./smart-sync-noai.sh --onto main
#   ./smart-sync-noai.sh --interactive
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

source "$SCRIPT_DIR/../lib/git-helpers.sh"

ONTO_BRANCH=""
INTERACTIVE=0
AUTO_SQUASH=0
STRATEGY=""
DRY_RUN=0
REPO="."

usage() {
  cat <<'EOF'
Usage: smart-sync-noai.sh [options]

Standard git sync (rebase) without AI.

Options:
  --onto <branch>             Sync onto branch (default: upstream)
  --interactive               Interactive rebase
  --auto-squash               Auto-squash fixup commits
  --strategy <name>           Rebase strategy (merge, ours, theirs)
  --dry-run                   Show what would be done
  -h, --help                  Show help

Examples:
  ./smart-sync-noai.sh
  ./smart-sync-noai.sh --onto main
  ./smart-sync-noai.sh --interactive

For AI-powered sync, use:
  ./smart-sync-copilot.sh
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --onto)
      ONTO_BRANCH="${2:-}"
      shift 2
      ;;
    --interactive)
      INTERACTIVE=1
      shift
      ;;
    --auto-squash)
      AUTO_SQUASH=1
      shift
      ;;
    --strategy)
      STRATEGY="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if ! validate_repo "$REPO"; then
  exit 1
fi

if ! is_clean_working_tree "$REPO"; then
  echo "ERROR: Working tree has uncommitted changes" >&2
  echo "Commit or stash changes before syncing" >&2
  exit 1
fi

current_branch="$(get_current_branch "$REPO")"
if [[ -z "$current_branch" ]]; then
  echo "ERROR: Detached HEAD state" >&2
  exit 1
fi

if [[ -n "$ONTO_BRANCH" ]]; then
  target="$ONTO_BRANCH"
else
  target="$(get_upstream_branch "$REPO")"
  if [[ -z "$target" ]]; then
    echo "ERROR: No upstream branch configured" >&2
    echo "Use --onto to specify target branch" >&2
    exit 1
  fi
fi

echo "Syncing $current_branch → $target"

rebase_args=()
[[ "$AUTO_SQUASH" -eq 1 ]] && rebase_args+=(--autosquash)
[[ -n "$STRATEGY" ]] && rebase_args+=(--strategy="$STRATEGY")
[[ "$INTERACTIVE" -eq 1 ]] && rebase_args+=(--interactive)

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: git rebase ${rebase_args[*]} $target"
  exit 0
fi

[[ "$INTERACTIVE" -eq 0 ]] && export GIT_EDITOR=:

if git -C "$REPO" rebase "${rebase_args[@]}" "$target"; then
  echo "=== Sync Complete ==="
else
  echo "ERROR: Rebase failed" >&2
  echo "  git rebase --abort  # to abort" >&2
  exit 1
fi
