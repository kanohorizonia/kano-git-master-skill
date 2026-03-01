#!/usr/bin/env bash
# Rebase current branch on top of Subversion changes
# Usage: ./rebase.sh [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Rebase the current branch on top of Subversion changes.

OPTIONS:
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Rebase on SVN changes
    $(basename "$0")

    # Dry run
    $(basename "$0") --dry-run

REQUIREMENTS:
    - git-svn installed
    - Repository cloned with git-svn

WHAT THIS DOES:
    - Fetches new SVN revisions
    - Rebases local commits on top of them
    - Maintains linear history

NOTES:
    - This is equivalent to: git svn fetch + git rebase git-svn
    - Requires clean working tree
    - May require conflict resolution
    - Use before dcommitting to SVN

WORKFLOW:
    1. Make local commits
    2. Rebase on SVN: ./svn/rebase.sh
    3. Resolve conflicts if any
    4. Commit to SVN: ./svn/dcommit.sh

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: Unknown option: $1" >&2
            usage
            exit 1
            ;;
    esac
done

# Check if we're in a git repository
if ! git rev-parse --git-dir > /dev/null 2>&1; then
    echo "Error: Not in a git repository" >&2
    exit 1
fi

# Check if this is a git-svn repository
if ! git config --get svn-remote.svn.url > /dev/null 2>&1; then
    echo "Error: This is not a git-svn repository" >&2
    echo "Clone with git-svn first: ./svn/clone.sh" >&2
    exit 1
fi

# Check for uncommitted changes
if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    echo "Error: You have uncommitted changes" >&2
    echo "Please commit or stash your changes first" >&2
    exit 1
fi

# Get SVN URL
SVN_URL=$(git config --get svn-remote.svn.url)

# Get current branch
CURRENT_BRANCH=$(git branch --show-current)

# Count local commits
LOCAL_COMMITS=$(git log --oneline git-svn..HEAD 2>/dev/null | wc -l || echo "0")

echo "Rebasing on Subversion changes..."
echo "  SVN URL: $SVN_URL"
echo "  Current Branch: $CURRENT_BRANCH"
echo "  Local Commits: $LOCAL_COMMITS"
echo ""

if [[ "$LOCAL_COMMITS" -gt 0 ]]; then
    echo "Local commits to rebase:"
    git log --oneline git-svn..HEAD
    echo ""
fi

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git svn rebase"
    echo ""
    echo "[DRY RUN] This would:"
    echo "  1. Fetch new SVN revisions"
    echo "  2. Import them as Git commits"
    if [[ "$LOCAL_COMMITS" -gt 0 ]]; then
        echo "  3. Rebase $LOCAL_COMMITS local commits on top"
    else
        echo "  3. Update current branch (no local commits to rebase)"
    fi
    echo ""
    echo "[DRY RUN] Note: May require conflict resolution"
    exit 0
fi

# Rebase on SVN
echo "Executing: git svn rebase"
echo ""

if git svn rebase; then
    echo ""
    echo "✓ Rebase completed successfully"
    echo ""
    if [[ "$LOCAL_COMMITS" -gt 0 ]]; then
        echo "Local commits rebased on top of SVN changes"
        echo ""
        echo "To commit changes: ./svn/dcommit.sh"
    else
        echo "Branch updated with SVN changes"
    fi
else
    echo ""
    echo "✗ Rebase failed or has conflicts" >&2
    echo ""
    echo "To resolve conflicts:" >&2
    echo "  1. Fix conflicts in affected files" >&2
    echo "  2. git add <resolved-files>" >&2
    echo "  3. git rebase --continue" >&2
    echo ""
    echo "To abort rebase:" >&2
    echo "  git rebase --abort" >&2
    exit 1
fi
