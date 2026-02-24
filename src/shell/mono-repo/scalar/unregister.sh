#!/usr/bin/env bash
# Unregister repository from Git Scalar
# Usage: ./unregister.sh [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Unregister the current repository from Git Scalar.

OPTIONS:
    --dry-run           Show what would be done without making changes
    -h, --help          Show this help message

EXAMPLES:
    # Unregister from Scalar
    $(basename "$0")

    # Dry run
    $(basename "$0") --dry-run

WHAT THIS DOES:
    - Removes Scalar registration
    - Disables background maintenance tasks
    - Keeps repository data intact
    - Reverts to standard Git configuration

WHAT IS PRESERVED:
    - All commits and history
    - All branches and tags
    - Working tree files
    - Git configuration (except Scalar-specific)

WHAT IS REMOVED:
    - Background maintenance schedule
    - Scalar-specific optimizations
    - Automatic prefetch/repack tasks

NOTES:
    - This is safe and reversible
    - Can re-register later with scalar/register.sh
    - Does not delete any repository data
    - Repository will use standard Git performance

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

# Check if git scalar is available
if ! command -v git > /dev/null 2>&1; then
    echo "Error: git command not found" >&2
    exit 1
fi

if ! git scalar --help > /dev/null 2>&1; then
    echo "Error: Git Scalar is not available" >&2
    echo "Git Scalar requires Git 2.38 or higher." >&2
    exit 1
fi

# Check if registered with Scalar
if ! git config --get-regexp 'maintenance\.' > /dev/null 2>&1; then
    echo "Repository is not registered with Scalar"
    echo "Nothing to do."
    exit 0
fi

# Get repository root
REPO_ROOT=$(git rev-parse --show-toplevel)

echo "Unregistering repository from Git Scalar..."
echo "  Repository: $REPO_ROOT"
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git scalar unregister"
    echo ""
    echo "[DRY RUN] Would remove:"
    echo "  - Background maintenance schedule"
    echo "  - Scalar-specific configuration"
    echo "  - Automatic optimization tasks"
    echo ""
    echo "[DRY RUN] Would preserve:"
    echo "  - All repository data"
    echo "  - All commits and history"
    echo "  - Working tree files"
    echo "  - Standard Git configuration"
    exit 0
fi

# Unregister from Scalar
echo "Executing: git scalar unregister"
echo ""
git scalar unregister

echo ""
echo "✓ Repository unregistered from Git Scalar"
echo ""
echo "Changes:"
echo "  ✓ Background maintenance disabled"
echo "  ✓ Scalar optimizations removed"
echo "  ✓ Repository reverted to standard Git"
echo ""
echo "Repository data preserved:"
echo "  ✓ All commits and history"
echo "  ✓ All branches and tags"
echo "  ✓ Working tree files"
echo ""
echo "To re-enable Scalar: ./scalar/register.sh"
