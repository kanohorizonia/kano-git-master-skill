#!/usr/bin/env bash
# Commit Git changes to Subversion
# Usage: ./dcommit.sh [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Commit (dcommit) Git changes to Subversion.

OPTIONS:
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Commit changes to SVN
    $(basename "$0")

    # Dry run
    $(basename "$0") --dry-run

REQUIREMENTS:
    - git-svn installed
    - Repository cloned with git-svn
    - Subversion write permissions

WHAT THIS DOES:
    - Converts Git commits to SVN revisions
    - Commits them to the Subversion repository
    - Updates git-svn metadata
    - Rebases local commits on top of SVN

NOTES:
    - Only commits changes not yet in SVN
    - Requires clean working tree
    - Commit messages become SVN log messages
    - This is like 'git push' for SVN

WORKFLOW:
    1. Make changes and commit to Git
    2. Fetch from SVN: ./svn/fetch.sh
    3. Rebase on SVN: ./svn/rebase.sh
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

# Count commits to dcommit
COMMITS_TO_DCOMMIT=$(git log --oneline git-svn..HEAD 2>/dev/null | wc -l || echo "0")

if [[ "$COMMITS_TO_DCOMMIT" -eq 0 ]]; then
    echo "No commits to dcommit"
    echo "All commits are already in Subversion"
    exit 0
fi

echo "Committing to Subversion..."
echo "  SVN URL: $SVN_URL"
echo "  Current Branch: $CURRENT_BRANCH"
echo "  Commits to Dcommit: $COMMITS_TO_DCOMMIT"
echo ""

# Show commits to dcommit
echo "Commits to dcommit:"
git log --oneline git-svn..HEAD
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git svn dcommit"
    echo ""
    echo "[DRY RUN] This would:"
    echo "  1. Convert $COMMITS_TO_DCOMMIT Git commits to SVN revisions"
    echo "  2. Commit them to: $SVN_URL"
    echo "  3. Update git-svn metadata"
    echo "  4. Rebase local commits on top of SVN"
    exit 0
fi

# Dcommit to SVN
echo "Executing: git svn dcommit"
echo ""

git svn dcommit

echo ""
echo "✓ Dcommit completed successfully"
echo ""
echo "Commits committed to Subversion: $COMMITS_TO_DCOMMIT"
echo ""
echo "To fetch updates: ./svn/fetch.sh"
