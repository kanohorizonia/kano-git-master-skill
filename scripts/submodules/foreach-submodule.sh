#!/usr/bin/env bash
# Execute a command in each submodule
# Usage: ./foreach-submodule.sh <command> [--recursive] [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
COMMAND=""
RECURSIVE=false
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") <command> [OPTIONS]

Execute a command in each submodule.

ARGUMENTS:
    command             Command to execute in each submodule

OPTIONS:
    --recursive         Execute in nested submodules as well
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Show status in all submodules
    $(basename "$0") "git status"

    # Pull latest changes in all submodules
    $(basename "$0") "git pull"

    # Show current branch in all submodules
    $(basename "$0") "git branch --show-current"

    # Execute in nested submodules too
    $(basename "$0") "git status" --recursive

    # Run a custom script
    $(basename "$0") "./build.sh"

    # Dry run
    $(basename "$0") "git pull" --dry-run

NOTES:
    - The command is executed in the context of each submodule directory
    - Use quotes around commands with spaces or special characters
    - The command has access to \$name, \$path, \$sha1, \$toplevel variables
    - Use --recursive to include nested submodules

SPECIAL VARIABLES:
    \$name              Name of the submodule
    \$path              Path to the submodule
    \$sha1              Current commit SHA of the submodule
    \$toplevel          Absolute path to the top-level repository

EXAMPLE WITH VARIABLES:
    $(basename "$0") 'echo "Submodule: \$name at \$path"'

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --recursive)
            RECURSIVE=true
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
        -*)
            echo "Error: Unknown option: $1" >&2
            usage
            exit 1
            ;;
        *)
            if [[ -z "$COMMAND" ]]; then
                COMMAND="$1"
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
if [[ -z "$COMMAND" ]]; then
    echo "Error: Command is required" >&2
    usage
    exit 1
fi

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

# Build git submodule foreach command
FOREACH_CMD="git submodule foreach"

if [[ "$RECURSIVE" == "true" ]]; then
    FOREACH_CMD="$FOREACH_CMD --recursive"
fi

FOREACH_CMD="$FOREACH_CMD '$COMMAND'"

# Show what will be done
echo "Executing command in submodules..."
echo "  Command: $COMMAND"
if [[ "$RECURSIVE" == "true" ]]; then
    echo "  Recursive: yes (including nested submodules)"
fi
echo ""

# List submodules
echo "Submodules:"
git submodule status | while read -r line; do
    echo "  $line"
done
echo ""

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: $FOREACH_CMD"
    exit 0
fi

# Execute command
echo "Executing: $FOREACH_CMD"
echo ""
echo "----------------------------------------"
echo ""

if [[ "$RECURSIVE" == "true" ]]; then
    git submodule foreach --recursive "$COMMAND"
else
    git submodule foreach "$COMMAND"
fi

echo ""
echo "----------------------------------------"
echo ""
echo "✓ Command executed in all submodules"
