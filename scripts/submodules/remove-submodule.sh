#!/usr/bin/env bash
# Remove a submodule safely
# Usage: ./remove-submodule.sh <path> [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
SUBMODULE_PATH=""
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") <path> [OPTIONS]

Remove a submodule safely from the repository.

ARGUMENTS:
    path                Path to the submodule (e.g., lib/mylib)

OPTIONS:
    --dry-run           Show what would be done without making changes
    -h, --help          Show this help message

EXAMPLES:
    # Remove a submodule
    $(basename "$0") lib/mylib

    # Dry run
    $(basename "$0") lib/mylib --dry-run

NOTES:
    - This removes the submodule from .gitmodules, .git/config, and the working tree
    - The submodule directory will be deleted
    - Changes must be committed manually
    - This is a safe operation that follows Git best practices

WHAT THIS DOES:
    1. Removes submodule entry from .gitmodules
    2. Removes submodule entry from .git/config
    3. Removes submodule from git index
    4. Removes submodule directory from working tree
    5. Removes submodule from .git/modules/

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
        -*)
            echo "Error: Unknown option: $1" >&2
            usage
            exit 1
            ;;
        *)
            if [[ -z "$SUBMODULE_PATH" ]]; then
                SUBMODULE_PATH="$1"
            else
                echo "Error: Too many arguments" >&2
                usage
                exit 1
            fi
            shift
            ;;
    esac
done

# Validate required arguments
if [[ -z "$SUBMODULE_PATH" ]]; then
    echo "Error: Submodule path is required" >&2
    usage
    exit 1
fi

# Check if we're in a git repository
if ! git rev-parse --git-dir > /dev/null 2>&1; then
    echo "Error: Not in a git repository" >&2
    exit 1
fi

# Check if .gitmodules exists
if [[ ! -f .gitmodules ]]; then
    echo "Error: No .gitmodules file found" >&2
    exit 1
fi

# Check if submodule exists
if ! git config --file .gitmodules --get "submodule.$SUBMODULE_PATH.path" > /dev/null 2>&1; then
    echo "Error: Submodule '$SUBMODULE_PATH' not found in .gitmodules" >&2
    exit 1
fi

# Get submodule info
SUBMODULE_URL=$(git config --file .gitmodules --get "submodule.$SUBMODULE_PATH.url" || echo "unknown")

echo "Removing submodule: $SUBMODULE_PATH"
echo "  URL: $SUBMODULE_URL"
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would perform the following steps:"
    echo "  1. git submodule deinit -f $SUBMODULE_PATH"
    echo "  2. git rm -f $SUBMODULE_PATH"
    echo "  3. rm -rf .git/modules/$SUBMODULE_PATH"
    echo ""
    echo "[DRY RUN] You would then need to commit the changes:"
    echo "  git commit -m \"Remove submodule $SUBMODULE_PATH\""
    exit 0
fi

# Step 1: Deinitialize the submodule
echo "Step 1: Deinitializing submodule..."
git submodule deinit -f "$SUBMODULE_PATH"

# Step 2: Remove from git index and working tree
echo "Step 2: Removing from git index and working tree..."
git rm -f "$SUBMODULE_PATH"

# Step 3: Remove from .git/modules
if [[ -d ".git/modules/$SUBMODULE_PATH" ]]; then
    echo "Step 3: Removing .git/modules/$SUBMODULE_PATH..."
    rm -rf ".git/modules/$SUBMODULE_PATH"
fi

echo ""
echo "✓ Submodule removed successfully"
echo ""
echo "Next steps:"
echo "  1. Review the changes: git status"
echo "  2. Commit the changes: git commit -m \"Remove submodule $SUBMODULE_PATH\""
