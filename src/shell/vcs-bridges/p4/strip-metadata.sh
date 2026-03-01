#!/usr/bin/env bash
#
# strip-metadata.sh - Strip git-p4 metadata from commits
#
# This script removes [git-p4: depot-paths = "...": change = ...] metadata
# from commit messages, which is useful when cherry-picking commits between
# P4-synced branches.
#

set -euo pipefail

COMMIT_RANGE=""
DRY_RUN=0
FORCE=0

usage() {
  cat << EOF
Usage: $(basename "$0") <commit|commit-range> [options]

Strip git-p4 metadata from commit messages.

Arguments:
  commit              Single commit hash
  commit-range        Commit range (e.g., abc123..def456, HEAD~3..HEAD)

Options:
  --force             Skip confirmation prompt
  --dry-run           Show what would be done
  -h, --help          Show this help

Examples:
  # Strip metadata from single commit
  $(basename "$0") abc123

  # Strip metadata from commit range
  $(basename "$0") abc123..def456

  # Strip metadata from last 3 commits
  $(basename "$0") HEAD~3..HEAD

Use Case:
  When cherry-picking commits from P4-synced release branches to main,
  the commits contain metadata like:
    [git-p4: depot-paths = "//DepotName/StreamName/Project/": change = 30000]
  
  This metadata should be stripped before pushing to avoid confusion.

Warning:
  This rewrites commit history! Original refs are backed up to refs/original/

EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --force) FORCE=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -*) echo "Error: Unknown option: $1" >&2; usage; exit 1 ;;
    *) COMMIT_RANGE="$1"; shift ;;
  esac
done

[[ -z "$COMMIT_RANGE" ]] && { echo "Error: Commit or commit range is required" >&2; usage; exit 1; }

# Check if we're in a git repository
if ! git rev-parse --git-dir &>/dev/null; then
  echo "[ERROR] Not in a git repository" >&2
  exit 1
fi

# Validate commit range
if ! git rev-parse "$COMMIT_RANGE" &>/dev/null; then
  echo "[ERROR] Invalid commit or commit range: $COMMIT_RANGE" >&2
  exit 1
fi

echo "[INFO] Stripping git-p4 metadata from: $COMMIT_RANGE"
echo ""

# Show affected commits
echo "[INFO] Affected commits:"
git log --oneline "$COMMIT_RANGE" 2>/dev/null || git log --oneline "$COMMIT_RANGE^..$COMMIT_RANGE"
echo ""

# Check if any commits have p4 metadata
if ! git log --format=%B "$COMMIT_RANGE" 2>/dev/null | grep -q "^\[git-p4:"; then
  if ! git log --format=%B "$COMMIT_RANGE^..$COMMIT_RANGE" 2>/dev/null | grep -q "^\[git-p4:"; then
    echo "[INFO] No git-p4 metadata found in specified commits"
    exit 0
  fi
fi

# Warning
if [[ $FORCE -eq 0 && $DRY_RUN -eq 0 ]]; then
  echo "[WARN] This will rewrite commit history!"
  echo "[WARN] Original refs will be backed up to refs/original/"
  echo "[WARN] Press Ctrl+C to cancel, or wait 5 seconds to continue..."
  sleep 5
fi

if [[ $DRY_RUN -eq 1 ]]; then
  echo "+ git filter-branch --msg-filter 'sed \"/^\\[git-p4: depot-paths/d\"' $COMMIT_RANGE"
  echo ""
  echo "[INFO] Dry-run complete. No changes made."
else
  echo "[INFO] Stripping metadata..."
  
  # Use filter-branch to remove metadata lines
  git filter-branch --msg-filter 'sed "/^\[git-p4: depot-paths/d"' "$COMMIT_RANGE"
  
  echo ""
  echo "[INFO] Metadata stripped successfully!"
  echo "[INFO] Original refs backed up to: refs/original/"
  echo ""
  echo "[INFO] To remove backup refs:"
  echo "  git update-ref -d refs/original/refs/heads/$(git rev-parse --abbrev-ref HEAD)"
  echo ""
  echo "[INFO] To restore original commits:"
  echo "  git reset --hard refs/original/refs/heads/$(git rev-parse --abbrev-ref HEAD)"
fi
