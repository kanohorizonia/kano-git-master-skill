#!/usr/bin/env bash
#
# smart-resolve-noai.sh - Standard conflict resolution without AI
#
# Purpose:
#   Display conflict information and guide manual resolution.
#   No AI-powered auto-resolution.
#
# Usage:
#   ./smart-resolve-noai.sh [options]
#
# Optional:
#   --file <path>               Show conflicts in specific file
#   -h, --help                  Show help
#
# Examples:
#   ./smart-resolve-noai.sh
#   ./smart-resolve-noai.sh --file src/main.ts
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TARGET_FILE=""
REPO="."

usage() {
  cat <<'EOF'
Usage: smart-resolve-noai.sh [options]

Standard conflict resolution without AI.

Optional:
  --file <path>               Show conflicts in specific file
  -h, --help                  Show help

Examples:
  ./smart-resolve-noai.sh
  ./smart-resolve-noai.sh --file src/main.ts

Manual Resolution Steps:
  1. Open conflicted files in your editor
  2. Search for conflict markers: <<<<<<<, =======, >>>>>>>
  3. Choose the correct version or merge manually
  4. Remove conflict markers
  5. Stage resolved files: git add <file>
  6. Continue: git rebase --continue OR git merge --continue

For AI-powered resolution, use:
  ./smart-resolve-copilot.sh
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --file)
      TARGET_FILE="${2:-}"
      shift 2
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

if ! git -C "$REPO" rev-parse --git-dir >/dev/null 2>&1; then
  echo "ERROR: Not a git repository" >&2
  exit 1
fi

if [[ -n "$TARGET_FILE" ]]; then
  if ! git -C "$REPO" diff --name-only --diff-filter=U | grep -qF "$TARGET_FILE"; then
    echo "No conflicts in: $TARGET_FILE"
    exit 0
  fi
  echo "Conflicts in: $TARGET_FILE"
  echo ""
  git -C "$REPO" diff "$TARGET_FILE"
else
  conflicted_files="$(git -C "$REPO" diff --name-only --diff-filter=U)"

  if [[ -z "$conflicted_files" ]]; then
    echo "No conflicts found"
    exit 0
  fi

  echo "Conflicted files:"
  echo "$conflicted_files" | while IFS= read -r file; do
    echo "  - $file"
  done
  echo ""
  echo "Resolution steps:"
  echo "  1. Edit the files above"
  echo "  2. Search for: <<<<<<<, =======, >>>>>>>"
  echo "  3. Choose or merge versions"
  echo "  4. Remove conflict markers"
  echo "  5. git add <file>"
  echo "  6. git rebase --continue OR git merge --continue"
  echo ""
  echo "For AI help: ./smart-resolve-copilot.sh"
fi
