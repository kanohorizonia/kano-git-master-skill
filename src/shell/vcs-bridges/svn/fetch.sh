#!/usr/bin/env bash
# Fetch changes from Subversion to Git
# Usage: ./fetch.sh [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Fetch changes from Subversion to the current Git repository.

OPTIONS:
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Fetch changes from SVN
    $(basename "$0")

    # Dry run
    $(basename "$0") --dry-run

REQUIREMENTS:
    - git-svn installed
    - Repository cloned with git-svn

WHAT THIS DOES:
    - Fetches new SVN revisions
    - Imports them as Git commits
    - Updates remote tracking branches
    - Does NOT update working tree

NOTES:
    - This is like 'git fetch' for SVN
    - Use 'git merge' or 'git rebase' to update working tree
    - Or use ./svn/rebase.sh to fetch and rebase in one step

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

# Get SVN URL
SVN_URL=$(git config --get svn-remote.svn.url)

# Get current branch
CURRENT_BRANCH=$(git branch --show-current)

echo "Fetching from Subversion..."
echo "  SVN URL: $SVN_URL"
echo "  Current Branch: $CURRENT_BRANCH"
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git svn fetch"
    echo ""
    echo "[DRY RUN] This would:"
    echo "  1. Fetch new SVN revisions"
    echo "  2. Import them as Git commits"
    echo "  3. Update remote tracking branches"
    echo "  4. NOT update working tree"
    echo ""
    echo "[DRY RUN] To update working tree after fetch:"
    echo "  git merge git-svn"
    echo "  or"
    echo "  git rebase git-svn"
    exit 0
fi

# Fetch from SVN
echo "Executing: git svn fetch"
echo ""

git svn fetch

echo ""
echo "✓ Fetch completed successfully"
echo ""
echo "To update working tree:"
echo "  git merge git-svn"
echo "  or"
echo "  git rebase git-svn"
echo "  or"
echo "  ./svn/rebase.sh"
echo ""
echo "To see new commits: git log --oneline git-svn"
