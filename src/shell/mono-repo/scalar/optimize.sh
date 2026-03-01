#!/usr/bin/env bash
# Run Git Scalar optimizations manually
# Usage: ./optimize.sh [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Run Git Scalar optimizations manually on the current repository.

OPTIONS:
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Run optimizations
    $(basename "$0")

    # Dry run
    $(basename "$0") --dry-run

WHAT THIS DOES:
    Runs all Scalar maintenance tasks immediately:
    1. Prefetch - Fetch new objects from remote
    2. Commit Graph - Update commit graph for faster traversal
    3. Loose Objects - Pack loose objects
    4. Incremental Repack - Repack objects incrementally
    5. Pack Refs - Pack references for faster access

WHEN TO USE:
    - After large changes (many commits, large files)
    - Before important operations (merge, rebase)
    - When repository feels slow
    - After cloning or pulling large changes

PERFORMANCE IMPACT:
    - May take several minutes for large repositories
    - Improves subsequent Git operations
    - Reduces disk usage
    - Speeds up git status, checkout, log

NOTES:
    - Repository must be registered with Scalar first
    - Safe to run multiple times
    - Runs in foreground (blocks until complete)

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
    echo "Error: Repository is not registered with Scalar" >&2
    echo "" >&2
    echo "Register first with: ./scalar/register.sh" >&2
    exit 1
fi

# Get repository root
REPO_ROOT=$(git rev-parse --show-toplevel)

echo "Running Git Scalar optimizations..."
echo "  Repository: $REPO_ROOT"
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git maintenance run --task=prefetch,commit-graph,loose-objects,incremental-repack,pack-refs"
    echo ""
    echo "[DRY RUN] Tasks that would run:"
    echo "  1. prefetch          - Fetch new objects from remote"
    echo "  2. commit-graph      - Update commit graph"
    echo "  3. loose-objects     - Pack loose objects"
    echo "  4. incremental-repack - Repack objects incrementally"
    echo "  5. pack-refs         - Pack references"
    echo ""
    echo "[DRY RUN] This may take several minutes for large repositories"
    exit 0
fi

# Get initial statistics
echo "Repository statistics (before):"
BEFORE_OBJECTS=$(git count-objects -v | grep '^count:' | awk '{print $2}')
BEFORE_PACKS=$(git count-objects -v | grep '^packs:' | awk '{print $2}')
BEFORE_SIZE_KB=$(git count-objects -v | grep '^size-pack:' | awk '{print $2}')
BEFORE_SIZE_MB=$((BEFORE_SIZE_KB / 1024))
echo "  Objects: $BEFORE_OBJECTS"
echo "  Packs: $BEFORE_PACKS"
echo "  Size: ${BEFORE_SIZE_MB} MB"
echo ""

# Run maintenance tasks
echo "Running maintenance tasks..."
echo ""

echo "Task 1/5: Prefetch (fetching new objects)..."
git maintenance run --task=prefetch 2>&1 | sed 's/^/  /'

echo ""
echo "Task 2/5: Commit Graph (updating commit graph)..."
git maintenance run --task=commit-graph 2>&1 | sed 's/^/  /'

echo ""
echo "Task 3/5: Loose Objects (packing loose objects)..."
git maintenance run --task=loose-objects 2>&1 | sed 's/^/  /'

echo ""
echo "Task 4/5: Incremental Repack (repacking objects)..."
git maintenance run --task=incremental-repack 2>&1 | sed 's/^/  /'

echo ""
echo "Task 5/5: Pack Refs (packing references)..."
git maintenance run --task=pack-refs 2>&1 | sed 's/^/  /'

echo ""
echo "✓ All optimization tasks completed"
echo ""

# Get final statistics
echo "Repository statistics (after):"
AFTER_OBJECTS=$(git count-objects -v | grep '^count:' | awk '{print $2}')
AFTER_PACKS=$(git count-objects -v | grep '^packs:' | awk '{print $2}')
AFTER_SIZE_KB=$(git count-objects -v | grep '^size-pack:' | awk '{print $2}')
AFTER_SIZE_MB=$((AFTER_SIZE_KB / 1024))
echo "  Objects: $AFTER_OBJECTS"
echo "  Packs: $AFTER_PACKS"
echo "  Size: ${AFTER_SIZE_MB} MB"

# Calculate improvements
OBJECTS_DIFF=$((BEFORE_OBJECTS - AFTER_OBJECTS))
PACKS_DIFF=$((BEFORE_PACKS - AFTER_PACKS))
SIZE_DIFF_MB=$((BEFORE_SIZE_MB - AFTER_SIZE_MB))

echo ""
echo "Improvements:"
if [[ $OBJECTS_DIFF -gt 0 ]]; then
    echo "  ✓ Reduced loose objects by $OBJECTS_DIFF"
fi
if [[ $PACKS_DIFF -gt 0 ]]; then
    echo "  ✓ Reduced pack count by $PACKS_DIFF"
elif [[ $PACKS_DIFF -lt 0 ]]; then
    echo "  • Pack count increased by ${PACKS_DIFF#-} (normal during incremental repack)"
fi
if [[ $SIZE_DIFF_MB -gt 0 ]]; then
    echo "  ✓ Reduced size by ${SIZE_DIFF_MB} MB"
elif [[ $SIZE_DIFF_MB -lt 0 ]]; then
    echo "  • Size increased by ${SIZE_DIFF_MB#-} MB (may happen with new fetches)"
fi

echo ""
echo "Check status with: ./scalar/status.sh"
