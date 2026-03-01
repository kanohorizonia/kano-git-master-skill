#!/usr/bin/env bash
# Show Git Scalar status and configuration
# Usage: ./status.sh [--format text|json]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
FORMAT="text"

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Show Git Scalar status and configuration for the current repository.

OPTIONS:
    --format <format>   Output format: text (default) or json
    -h, --help          Show this help message

EXAMPLES:
    # Show status (text format)
    $(basename "$0")

    # Show status (JSON format)
    $(basename "$0") --format json

WHAT THIS SHOWS:
    - Whether Scalar is registered
    - Enabled optimizations
    - Background maintenance status
    - Repository statistics
    - Configuration values

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --format)
            FORMAT="$2"
            if [[ "$FORMAT" != "text" && "$FORMAT" != "json" ]]; then
                echo "Error: Invalid format: $FORMAT (must be text or json)" >&2
                exit 1
            fi
            shift 2
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
    if [[ "$FORMAT" == "json" ]]; then
        echo '{"error": "Git Scalar not available", "scalar_available": false}'
    else
        echo "Error: Git Scalar is not available" >&2
        echo "Git Scalar requires Git 2.38 or higher." >&2
    fi
    exit 1
fi

# Get repository root
REPO_ROOT=$(git rev-parse --show-toplevel)

# Check if registered with Scalar
SCALAR_REGISTERED=false
if git config --get-regexp 'maintenance\.' > /dev/null 2>&1; then
    SCALAR_REGISTERED=true
fi

# Get configuration values
PARTIAL_CLONE=$(git config --get remote.origin.promisor 2>/dev/null || echo "false")
SPARSE_CHECKOUT=$(git config --get core.sparseCheckout 2>/dev/null || echo "false")
FSMONITOR=$(git config --get core.fsmonitor 2>/dev/null || echo "false")
MULTIPACK_INDEX=$(git config --get core.multiPackIndex 2>/dev/null || echo "false")
COMMIT_GRAPH=$(git config --get core.commitGraph 2>/dev/null || echo "false")

# Get maintenance schedule
MAINTENANCE_ENABLED=$(git config --get maintenance.auto 2>/dev/null || echo "false")
PREFETCH_SCHEDULE=$(git config --get maintenance.prefetch.schedule 2>/dev/null || echo "none")
COMMIT_GRAPH_SCHEDULE=$(git config --get maintenance.commit-graph.schedule 2>/dev/null || echo "none")
LOOSE_OBJECTS_SCHEDULE=$(git config --get maintenance.loose-objects.schedule 2>/dev/null || echo "none")
INCREMENTAL_REPACK_SCHEDULE=$(git config --get maintenance.incremental-repack.schedule 2>/dev/null || echo "none")

# Get repository statistics
OBJECT_COUNT=$(git count-objects -v | grep '^count:' | awk '{print $2}')
PACK_COUNT=$(git count-objects -v | grep '^packs:' | awk '{print $2}')
SIZE_KB=$(git count-objects -v | grep '^size-pack:' | awk '{print $2}')
SIZE_MB=$((SIZE_KB / 1024))

# Output based on format
if [[ "$FORMAT" == "json" ]]; then
    cat <<EOF
{
  "repository": "$REPO_ROOT",
  "scalar_registered": $SCALAR_REGISTERED,
  "optimizations": {
    "partial_clone": "$PARTIAL_CLONE",
    "sparse_checkout": "$SPARSE_CHECKOUT",
    "fsmonitor": "$FSMONITOR",
    "multipack_index": "$MULTIPACK_INDEX",
    "commit_graph": "$COMMIT_GRAPH"
  },
  "maintenance": {
    "enabled": "$MAINTENANCE_ENABLED",
    "schedules": {
      "prefetch": "$PREFETCH_SCHEDULE",
      "commit_graph": "$COMMIT_GRAPH_SCHEDULE",
      "loose_objects": "$LOOSE_OBJECTS_SCHEDULE",
      "incremental_repack": "$INCREMENTAL_REPACK_SCHEDULE"
    }
  },
  "statistics": {
    "object_count": $OBJECT_COUNT,
    "pack_count": $PACK_COUNT,
    "size_mb": $SIZE_MB
  }
}
EOF
else
    echo "Git Scalar Status"
    echo "================="
    echo ""
    echo "Repository: $REPO_ROOT"
    echo ""
    
    if [[ "$SCALAR_REGISTERED" == "true" ]]; then
        echo "Status: ✓ Registered with Scalar"
    else
        echo "Status: ✗ Not registered with Scalar"
        echo ""
        echo "To register: ./scalar/register.sh"
        exit 0
    fi
    
    echo ""
    echo "Optimizations:"
    echo "  Partial Clone:     $PARTIAL_CLONE"
    echo "  Sparse Checkout:   $SPARSE_CHECKOUT"
    echo "  FSMonitor:         $FSMONITOR"
    echo "  Multi-pack Index:  $MULTIPACK_INDEX"
    echo "  Commit Graph:      $COMMIT_GRAPH"
    
    echo ""
    echo "Background Maintenance:"
    echo "  Enabled:           $MAINTENANCE_ENABLED"
    echo "  Prefetch:          $PREFETCH_SCHEDULE"
    echo "  Commit Graph:      $COMMIT_GRAPH_SCHEDULE"
    echo "  Loose Objects:     $LOOSE_OBJECTS_SCHEDULE"
    echo "  Incremental Repack: $INCREMENTAL_REPACK_SCHEDULE"
    
    echo ""
    echo "Repository Statistics:"
    echo "  Objects:           $OBJECT_COUNT"
    echo "  Packs:             $PACK_COUNT"
    echo "  Size:              ${SIZE_MB} MB"
    
    echo ""
    echo "Commands:"
    echo "  Run optimization:  ./scalar/optimize.sh"
    echo "  Unregister:        ./scalar/unregister.sh"
fi
