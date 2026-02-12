#!/usr/bin/env bash
# Sync (pull) changes from Perforce to Git
# Usage: ./sync.sh [--rebase] [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source "$SCRIPT_DIR/../../lib/p4-helpers.sh"

# Default options
REBASE=false
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Sync (pull) changes from Perforce to the current Git repository.

OPTIONS:
    --rebase            Rebase local commits on top of synced changes
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Sync changes from Perforce
    $(basename "$0")

    # Sync and rebase local commits
    $(basename "$0") --rebase

    # Dry run
    $(basename "$0") --dry-run

REQUIREMENTS:
    - Python 3.x (git-p4 requires Python 3)
    - git-p4 installed
    - Repository cloned with git-p4
    - P4PORT environment variable set

WHAT THIS DOES:
    - Fetches new changelists from Perforce
    - Imports them as Git commits
    - Updates the current branch
    - Optionally rebases local commits

NOTES:
    - Use --rebase if you have local commits
    - Without --rebase, creates a merge commit
    - Requires clean working tree

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rebase)
            REBASE=true
            shift
            ;;
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

echo "Syncing from Perforce..."
echo "  Depot Path: $DEPOT_PATH"
echo "  Current Branch: $CURRENT_BRANCH"
echo "  Rebase: $REBASE"
echo "  P4PORT: ${P4PORT:-not set}"
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    if [[ "$REBASE" == "true" ]]; then
        echo "[DRY RUN] Would execute: git p4 rebase"
    else
        echo "[DRY RUN] Would execute: git p4 sync"
    fi
    echo ""
    echo "[DRY RUN] This would:"
    echo "  1. Fetch new changelists from Perforce"
    echo "  2. Import them as Git commits"
    echo "  3. Update current branch"
    if [[ "$REBASE" == "true" ]]; then
        echo "  4. Rebase local commits on top"
    fi
    exit 0
fi

# Sync from Perforce
if [[ "$REBASE" == "true" ]]; then
    echo "Executing: git p4 rebase"
    echo ""
    git p4 rebase
else
    echo "Executing: git p4 sync"
    echo ""
    git p4 sync
fi

echo ""
echo "✓ Sync completed successfully"
echo ""
echo "To see new commits: git log --oneline -10"
echo "To submit changes: ./p4/submit.sh"
