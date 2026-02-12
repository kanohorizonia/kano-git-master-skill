#!/usr/bin/env bash
# Clone a Perforce depot to a Git repository
# Usage: ./clone.sh <depot-path> [<directory>] [--branch <branch>] [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source "$SCRIPT_DIR/../../lib/p4-helpers.sh"

# Default options
DEPOT_PATH=""
DIRECTORY=""
BRANCH="master"
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") <depot-path> [directory] [OPTIONS]

Clone a Perforce depot to a Git repository using git-p4.

ARGUMENTS:
    depot-path          Perforce depot path (e.g., //depot/project/...)
    directory           Target directory (optional, defaults to last path component)

OPTIONS:
    --branch <name>     Branch name for imported commits (default: master)
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Clone a depot
    $(basename "$0") //depot/myproject/...

    # Clone to specific directory
    $(basename "$0") //depot/myproject/... my-project

    # Clone to specific branch
    $(basename "$0") //depot/myproject/... --branch main

    # Dry run
    $(basename "$0") //depot/myproject/... --dry-run

REQUIREMENTS:
    - Python 3.x (git-p4 requires Python 3)
    - git-p4 installed
    - P4PORT environment variable set
    - Perforce credentials configured

ENVIRONMENT VARIABLES:
    P4PORT              Perforce server (e.g., perforce:1666)
    P4USER              Perforce username
    P4CLIENT            Perforce client workspace (optional)

NOTES:
    - This creates a new Git repository
    - All Perforce history is imported
    - May take a long time for large depots
    - Commits include git-p4 metadata

WHAT IS IMPORTED:
    - All changelists from the depot path
    - File history and content
    - Commit messages
    - Author information
    - Timestamps

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --branch)
            BRANCH="$2"
            shift 2
            ;;
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
            if [[ -z "$DEPOT_PATH" ]]; then
                DEPOT_PATH="$1"
            elif [[ -z "$DIRECTORY" ]]; then
                DIRECTORY="$1"
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
if [[ -z "$DEPOT_PATH" ]]; then
    echo "Error: Depot path is required" >&2
    usage
    exit 1
fi

# Validate depot path format
if [[ ! "$DEPOT_PATH" =~ ^//.*\.\.\.$ ]]; then
    echo "Error: Invalid depot path format: $DEPOT_PATH" >&2
    echo "Depot path should be in format: //depot/path/..." >&2
    exit 1
fi

# Default directory from depot path
if [[ -z "$DIRECTORY" ]]; then
    # Extract last component before /...
    DIRECTORY=$(echo "$DEPOT_PATH" | sed 's|/\.\.\.$||' | sed 's|.*/||')
fi

# Validate environment
validate_p4_environment || exit 1

echo "Cloning Perforce depot to Git repository..."
echo "  Depot Path: $DEPOT_PATH"
echo "  Directory: $DIRECTORY"
echo "  Branch: $BRANCH"
echo "  P4PORT: ${P4PORT:-not set}"
echo "  P4USER: ${P4USER:-not set}"
echo ""

# Check if directory already exists
if [[ -d "$DIRECTORY" ]]; then
    echo "Error: Directory already exists: $DIRECTORY" >&2
    exit 1
fi

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: git p4 clone --branch=$BRANCH $DEPOT_PATH $DIRECTORY"
    echo ""
    echo "[DRY RUN] This would:"
    echo "  1. Create directory: $DIRECTORY"
    echo "  2. Initialize Git repository"
    echo "  3. Import all Perforce changelists"
    echo "  4. Create branch: $BRANCH"
    echo "  5. Configure git-p4 settings"
    echo ""
    echo "[DRY RUN] Note: This may take a long time for large depots"
    exit 0
fi

# Clone the depot
echo "Executing: git p4 clone --branch=$BRANCH $DEPOT_PATH $DIRECTORY"
echo ""
echo "This may take a long time for large depots..."
echo ""

git p4 clone --branch="$BRANCH" "$DEPOT_PATH" "$DIRECTORY"

echo ""
echo "✓ Clone completed successfully"
echo ""
echo "Repository created at: $DIRECTORY"
echo ""
echo "Next steps:"
echo "  cd $DIRECTORY"
echo "  git log --oneline"
echo ""
echo "To sync updates: ./p4/sync.sh"
echo "To submit changes: ./p4/submit.sh"
