#!/usr/bin/env bash
# Update all submodules to their latest commits
# Usage: ./update-submodules.sh [--recursive] [--remote] [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
RECURSIVE=false
REMOTE=false
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Update all submodules to their latest commits.

OPTIONS:
    --recursive         Update submodules recursively (nested submodules)
    --remote            Update to latest commit on remote branch (instead of recorded commit)
    --dry-run           Show what would be done without making changes
    -h, --help          Show this help message

EXAMPLES:
    # Update all submodules to recorded commits
    $(basename "$0")

    # Update recursively (including nested submodules)
    $(basename "$0") --recursive

    # Update to latest remote commits
    $(basename "$0") --remote

    # Update recursively to latest remote commits
    $(basename "$0") --recursive --remote

    # Dry run
    $(basename "$0") --remote --dry-run

NOTES:
    - Without --remote: Updates to the commit recorded in the parent repo
    - With --remote: Updates to the latest commit on the tracked branch
    - Use --recursive for repos with nested submodules
    - Requires clean working tree (uncommitted changes will be stashed)

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --recursive)
            RECURSIVE=true
            shift
            ;;
        --remote)
            REMOTE=true
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

# Check if there are any submodules
if ! git config --file .gitmodules --get-regexp path > /dev/null 2>&1; then
    echo "No submodules found in this repository"
    exit 0
fi

# Build git submodule update command
UPDATE_CMD="git submodule update --init"

if [[ "$RECURSIVE" == "true" ]]; then
    UPDATE_CMD="$UPDATE_CMD --recursive"
fi

if [[ "$REMOTE" == "true" ]]; then
    UPDATE_CMD="$UPDATE_CMD --remote"
fi

# Show what will be done
echo "Updating submodules..."
if [[ "$RECURSIVE" == "true" ]]; then
    echo "  - Recursive: yes (including nested submodules)"
fi
if [[ "$REMOTE" == "true" ]]; then
    echo "  - Remote: yes (latest commits from tracked branches)"
else
    echo "  - Remote: no (commits recorded in parent repo)"
fi
echo ""

# List submodules
echo "Submodules to update:"
git submodule status | while read -r line; do
    echo "  $line"
done
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: $UPDATE_CMD"
    exit 0
fi

# Execute update
echo "Executing: $UPDATE_CMD"
echo ""
$UPDATE_CMD

echo ""
echo "✓ Submodules updated successfully"

# Show updated status
echo ""
echo "Updated submodule status:"
git submodule status
