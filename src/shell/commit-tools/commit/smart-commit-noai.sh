#!/usr/bin/env bash
#
# smart-commit-noai.sh - Standard commit without AI
#
# Purpose:
#   Commit changes using standard git commit without AI.
#   Requires manual commit message via -m flag.
#
# Usage:
#   ./smart-commit-noai.sh -m "message" [options]
#
# Required:
#   -m, --message <text>        Commit message
#
# Optional:
#   --repos <paths>             Only process specific repos (comma-separated)
#   --dry-run                   Show what would be done
#   -h, --help                  Show help
#
# Examples:
#   ./smart-commit-noai.sh -m "Fix bug"
#   ./smart-commit-noai.sh -m "Add feature" --repos "."
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MESSAGE=""
REPO_FILTER=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: smart-commit-noai.sh -m "message" [options]

Standard git commit without AI.

Required:
  -m, --message <text>        Commit message

Optional:
  --repos <paths>             Only process specific repos (comma-separated)
  --dry-run                   Show what would be done
  -h, --help                  Show help

Examples:
  ./smart-commit-noai.sh -m "Fix bug"
  ./smart-commit-noai.sh -m "Add feature" --repos "."

Note:
  For AI-generated commit messages, use:
    ./smart-commit-copilot.sh
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -m|--message)
      MESSAGE="${2:-}"
      shift 2
      ;;
    --repos)
      REPO_FILTER="${2:-}"
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
      echo "ERROR: Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$MESSAGE" ]]; then
  echo "ERROR: Commit message is required (-m)" >&2
  usage >&2
  exit 1
fi

ROOT="$(cd "$SCRIPT_DIR/../../.." && git rev-parse --show-toplevel 2>/dev/null || pwd)"

git -C "$ROOT" add -A 2>/dev/null || true

if git -C "$ROOT" diff --cached --quiet 2>/dev/null; then
  echo "No changes to commit"
  exit 0
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would commit: $MESSAGE"
  git -C "$ROOT" diff --cached --stat
  exit 0
fi

if git -C "$ROOT" commit -m "$MESSAGE"; then
  echo "=== Commit Complete ==="
else
  echo "ERROR: Commit failed" >&2
  exit 1
fi
