#!/usr/bin/env bash
# Rebase current branch on top of Perforce changes
# Usage: ./rebase.sh [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source "$SCRIPT_DIR/../../lib/p4-helpers.sh"

# Default options
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Rebase the current branch on top of Perforce changes.

OPTIONS:
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Rebase on Perforce changes
    $(basename "$0")

    # Dry run
    $(basename "$0") --dry-run

REQUIREMENTS:
    - Python 3.x (git-p4 requires Python 3)
    - git-p4 installed
    - Repository cloned with git-p4
    - P4PORT environment variable set

WHAT THIS DOES:
    - Fetches new changelists from Perforce
    - Rebases local commits on top of them
    - Maintains linear history

NOTES:
    - This is equivalent to: git p4 sync --rebase
    - Requires clean working tree
    - May require conflict resolution
    - Use before submitting to Perforce

WORKFLOW:
    1. Make local commits
    2. Rebase on Perforce: ./p4/rebase.sh
    3. Resolve conflicts if any
    4. Submit to Perforce: ./p4/submit.sh

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

# Check if this is a git-p4 repository
if ! git config --get git-p4.depotPath > /dev/null 2>&1; then
    echo "Error: This is not a git-p4 repository" >&2
    echo "Clone with git-p4 first: ./p4/clone.sh" >&2
    exit 1
fi

# Validate environment
validate_p4_environment || exit 1

# Check for uncommitted changes
if ! git diff-index --quiet HEAD -- 2>/dev/null; then
    echo "Error: You have uncommitted changes" >&2
    echo "Please commit or stash your changes first" >&2
    exit 1
fi

# Get current branch
CURRENT_BRANCH=$(git branch --show-current)

# Get depot path
DEPOT_PATH=$(get_p4_depot_path)

# Count local commits
LOCAL_COMMITS=$(git log --oneline origin/p4/master..HEAD 2>/dev/null | wc -l || echo "0")

echo "Rebasing on Perforce changes..."
echo "  Depot Path: $DEPOT_PATH"
echo "  Current Branch: $CURRENT_BRANCH"
echo "  Local Commits: $LOCAL_COMMITS"
echo "  P4PORT: ${P4PORT:-not set}"
echo ""

if [[ "$LOCAL_COMMITS" -gt 0 ]]; then
    echo "Local commits to rebase:"
    git log --oneline origin/p4/master..HEAD 2>/dev/null || git log --oneline -"$LOCAL_COMMITS"
    echo ""
fi

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git p4 rebase"
    echo ""
    echo "[DRY RUN] This would:"
    echo "  1. Fetch new changelists from Perforce"
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

# Rebase on Perforce
echo "Executing: git p4 rebase"
echo ""

if git p4 rebase; then
    echo ""
    echo "✓ Rebase completed successfully"
    echo ""
    if [[ "$LOCAL_COMMITS" -gt 0 ]]; then
        echo "Local commits rebased on top of Perforce changes"
        echo ""
        echo "To submit changes: ./p4/submit.sh"
    else
        echo "Branch updated with Perforce changes"
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
