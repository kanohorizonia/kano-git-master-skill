#!/usr/bin/env bash
# Clone a Subversion repository to Git
# Usage: ./clone.sh <svn-url> [<directory>] [--trunk <path>] [--branches <path>] [--tags <path>] [--dry-run]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

# Default options
SVN_URL=""
DIRECTORY=""
TRUNK="trunk"
BRANCHES="branches"
TAGS="tags"
STANDARD_LAYOUT=true
DRY_RUN=false

usage() {
    cat <<EOF
Usage: $(basename "$0") <svn-url> [directory] [OPTIONS]

Clone a Subversion repository to a Git repository using git-svn.

ARGUMENTS:
    svn-url             Subversion repository URL
    directory           Target directory (optional, defaults to repo name)

OPTIONS:
    --trunk <path>      Path to trunk (default: trunk)
    --branches <path>   Path to branches (default: branches)
    --tags <path>       Path to tags (default: tags)
    --no-standard       Don't use standard layout (trunk/branches/tags)
    --dry-run           Show what would be done without executing
    -h, --help          Show this help message

EXAMPLES:
    # Clone with standard layout
    $(basename "$0") https://svn.example.com/repo

    # Clone to specific directory
    $(basename "$0") https://svn.example.com/repo my-project

    # Clone with custom layout
    $(basename "$0") https://svn.example.com/repo \\
        --trunk main --branches feature --tags releases

    # Clone without standard layout (single branch)
    $(basename "$0") https://svn.example.com/repo/trunk --no-standard

    # Dry run
    $(basename "$0") https://svn.example.com/repo --dry-run

REQUIREMENTS:
    - git-svn installed
    - Subversion credentials configured

STANDARD LAYOUT:
    By default, assumes SVN repository has standard layout:
      trunk/       - Main development branch
      branches/    - Feature/release branches
      tags/        - Release tags

WHAT IS IMPORTED:
    - All SVN revisions
    - Commit messages
    - Author information
    - Timestamps
    - Branches and tags (if standard layout)

NOTES:
    - This creates a new Git repository
    - May take a long time for large repositories
    - Commits include git-svn metadata

EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --trunk)
            TRUNK="$2"
            shift 2
            ;;
        --branches)
            BRANCHES="$2"
            shift 2
            ;;
        --tags)
            TAGS="$2"
            shift 2
            ;;
        --no-standard)
            STANDARD_LAYOUT=false
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
            if [[ -z "$SVN_URL" ]]; then
                SVN_URL="$1"
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
if [[ -z "$SVN_URL" ]]; then
    echo "Error: SVN URL is required" >&2
    usage
    exit 1
fi

# Default directory from URL
if [[ -z "$DIRECTORY" ]]; then
    DIRECTORY=$(basename "$SVN_URL")
fi

# Check if git-svn is available
if ! git svn --version > /dev/null 2>&1; then
    echo "Error: git-svn is not available" >&2
    echo "" >&2
    echo "Installation:" >&2
    echo "  - Windows: Included with Git for Windows" >&2
    echo "  - macOS: brew install git-svn" >&2
    echo "  - Linux: apt-get install git-svn or yum install git-svn" >&2
    exit 1
fi

echo "Cloning Subversion repository to Git..."
echo "  SVN URL: $SVN_URL"
echo "  Directory: $DIRECTORY"
if [[ "$STANDARD_LAYOUT" == "true" ]]; then
    echo "  Layout: Standard (trunk=$TRUNK, branches=$BRANCHES, tags=$TAGS)"
else
    echo "  Layout: Non-standard (single branch)"
fi
echo ""

# Check if directory already exists
if [[ -d "$DIRECTORY" ]]; then
    echo "Error: Directory already exists: $DIRECTORY" >&2
    exit 1
fi

# Build git svn clone command
if [[ "$STANDARD_LAYOUT" == "true" ]]; then
    CLONE_CMD="git svn clone --trunk=$TRUNK --branches=$BRANCHES --tags=$TAGS $SVN_URL $DIRECTORY"
else
    CLONE_CMD="git svn clone $SVN_URL $DIRECTORY"
fi

if [[ "$DRY_RUN" == "true" ]]; then
    echo "[DRY RUN] Would execute: $CLONE_CMD"
    echo ""
    echo "[DRY RUN] This would:"
    echo "  1. Create directory: $DIRECTORY"
    echo "  2. Initialize Git repository"
    echo "  3. Import all SVN revisions"
    if [[ "$STANDARD_LAYOUT" == "true" ]]; then
        echo "  4. Create Git branches from SVN branches"
        echo "  5. Create Git tags from SVN tags"
    fi
    echo ""
    echo "[DRY RUN] Note: This may take a long time for large repositories"
    exit 0
fi

# Clone the repository
echo "Executing: $CLONE_CMD"
echo ""
echo "This may take a long time for large repositories..."
echo ""

$CLONE_CMD

echo ""
echo "✓ Clone completed successfully"
echo ""
echo "Repository created at: $DIRECTORY"
echo ""
echo "Next steps:"
echo "  cd $DIRECTORY"
echo "  git log --oneline"
echo ""
echo "To fetch updates: ./svn/fetch.sh"
echo "To commit changes: ./svn/dcommit.sh"
