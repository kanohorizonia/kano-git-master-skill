#!/usr/bin/env bash
# Submit (push) Git commits to Perforce
# Usage: ./submit.sh [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source "$SCRIPT_DIR/../../lib/p4-helpers.sh"

# Default options
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Submit (push) Git commits to Perforce.

OPTIONS:
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Submit commits to Perforce
    $(basename "$0")

    # Dry run
    $(basename "$0") --dry-run

REQUIREMENTS:
    - Python 3.x (git-p4 requires Python 3)
    - git-p4 installed
    - Repository cloned with git-p4
    - P4PORT environment variable set
    - Perforce write permissions

WHAT THIS DOES:
    - Converts Git commits to Perforce changelists
    - Submits them to the Perforce depot
    - Updates git-p4 metadata

NOTES:
    - Only submits commits not yet in Perforce
    - Requires clean working tree
    - May require Perforce client workspace
    - Commit messages become changelist descriptions

WORKFLOW:
    1. Make changes and commit to Git
    2. Sync from Perforce: ./p4/sync.sh --rebase
    3. Submit to Perforce: ./p4/submit.sh

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

# Count commits to submit
COMMITS_TO_SUBMIT=$(git log --oneline origin/p4/master..HEAD 2>/dev/null | wc -l || echo "0")

if [[ "$COMMITS_TO_SUBMIT" -eq 0 ]]; then
    echo "No commits to submit"
    echo "All commits are already in Perforce"
    exit 0
fi

echo "Submitting commits to Perforce..."
echo "  Depot Path: $DEPOT_PATH"
echo "  Current Branch: $CURRENT_BRANCH"
echo "  Commits to Submit: $COMMITS_TO_SUBMIT"
echo "  P4PORT: ${P4PORT:-not set}"
echo ""

# Show commits to submit
echo "Commits to submit:"
git log --oneline origin/p4/master..HEAD 2>/dev/null || git log --oneline -"$COMMITS_TO_SUBMIT"
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git p4 submit"
    echo ""
    echo "[DRY RUN] This would:"
    echo "  1. Convert $COMMITS_TO_SUBMIT Git commits to Perforce changelists"
    echo "  2. Submit them to: $DEPOT_PATH"
    echo "  3. Update git-p4 metadata"
    echo ""
    echo "[DRY RUN] Note: You may be prompted to edit changelist descriptions"
    exit 0
fi

# Submit to Perforce
echo "Executing: git p4 submit"
echo ""
echo "Note: You may be prompted to edit changelist descriptions"
echo ""

git p4 submit

echo ""
echo "✓ Submit completed successfully"
echo ""
echo "Commits submitted to Perforce: $COMMITS_TO_SUBMIT"
echo ""
echo "To sync updates: ./p4/sync.sh"
