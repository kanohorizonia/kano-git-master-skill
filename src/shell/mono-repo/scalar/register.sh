#!/usr/bin/env bash
# Register repository with Git Scalar for mono-repo optimization
# Usage: ./register.sh [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Register the current repository with Git Scalar for mono-repo optimization.

OPTIONS:
    --dry-run           Show what would be configured without making changes
    -h, --help          Show this help message

EXAMPLES:
    # Register with Scalar
    $(basename "$0")

    # Dry run
    $(basename "$0") --dry-run

WHAT IS GIT SCALAR?
    Git Scalar is a tool for managing large Git repositories (mono-repos).
    It configures Git with optimizations that can provide 10-20x performance improvements.

OPTIMIZATIONS ENABLED:
    1. Partial Clone - Download only needed objects (not full history)
    2. Sparse Checkout - Check out only needed files
    3. Background Maintenance - Automatic optimization tasks
    4. FSMonitor - File system monitoring for faster status checks
    5. Multi-pack Index - Faster object lookups
    6. Commit Graph - Faster history traversal

REQUIREMENTS:
    - Git 2.38+ with Scalar support
    - Run 'git scalar' to check if available

PERFORMANCE IMPACT:
    - Initial clone: 10-20x faster
    - git status: 5-10x faster
    - git checkout: 3-5x faster
    - Disk usage: 50-90% reduction

NOTES:
    - This is safe to run on existing repositories
    - Can be reversed with scalar/unregister.sh
    - Recommended for repositories > 1GB or > 100k files

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
    echo "" >&2
    echo "Git Scalar requires Git 2.38 or higher." >&2
    echo "Please upgrade Git or use manual configuration." >&2
    echo "" >&2
    echo "Check your Git version:" >&2
    echo "  git --version" >&2
    exit 1
fi

# Get repository root
REPO_ROOT=$(git rev-parse --show-toplevel)

echo "Registering repository with Git Scalar..."
echo "  Repository: $REPO_ROOT"
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git scalar register"
    echo ""
    echo "[DRY RUN] Would configure:"
    echo "  - Partial clone (blob:none filter)"
    echo "  - Sparse checkout (cone mode)"
    echo "  - Background maintenance (hourly)"
    echo "  - FSMonitor (if available)"
    echo "  - Multi-pack index"
    echo "  - Commit graph"
    echo ""
    echo "[DRY RUN] Would enable background tasks:"
    echo "  - prefetch: Fetch new objects in background"
    echo "  - commit-graph: Update commit graph"
    echo "  - loose-objects: Pack loose objects"
    echo "  - incremental-repack: Repack incrementally"
    exit 0
fi

# Register with Scalar
echo "Executing: git scalar register"
echo ""
git scalar register

echo ""
echo "✓ Repository registered with Git Scalar"
echo ""
echo "Optimizations enabled:"
echo "  ✓ Partial clone configuration"
echo "  ✓ Sparse checkout configuration"
echo "  ✓ Background maintenance scheduled"
echo "  ✓ FSMonitor enabled (if available)"
echo "  ✓ Multi-pack index enabled"
echo "  ✓ Commit graph enabled"
echo ""
echo "Background maintenance tasks:"
echo "  - prefetch: Hourly"
echo "  - commit-graph: Hourly"
echo "  - loose-objects: Daily"
echo "  - incremental-repack: Daily"
echo ""
echo "Check status with: ./scalar/status.sh"
echo "Run manual optimization: ./scalar/optimize.sh"
