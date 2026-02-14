#!/usr/bin/env bash
#
# get-version-info.sh - Get version information from repository
#
# Purpose:
#   Extract version information from Git, git-p4, or git-svn repositories
#   Supports multiple output formats and can export as environment variables
#
# Usage:
#   ./get-version-info.sh [path] [options]
#
# Arguments:
#   path              Repository path (default: current directory)
#
# Options:
#   --format <json|env|text>  Output format (default: text)
#   --export                  Output as export statements (for eval)
#   --detect-only             Only detect and print VCS type
#   -h, --help                Show help
#
# Examples:
#   # Get version info for current directory
#   ./get-version-info.sh
#
#   # Get version info as JSON
#   ./get-version-info.sh --format json
#
#   # Export as environment variables
#   eval "$(./get-version-info.sh --export)"
#
#   # Detect VCS type
#   ./get-version-info.sh --detect-only
#
#   # Get version info for specific repo
#   ./get-version-info.sh /path/to/repo
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/version-helpers.sh"

# Default values
REPO_PATH="."
OUTPUT_FORMAT="text"
EXPORT_MODE=0
DETECT_ONLY=0
REVISION_OFFSET=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") [path] [options]

Get version information from Git, git-p4, or git-svn repository.

Arguments:
  path              Repository path (default: current directory)

Options:
  --format <json|env|text>  Output format (default: text)
  --export                  Output as export statements (for eval)
  --offset <number>         Revision offset to add (default: 0)
  --detect-only             Only detect and print VCS type
  -h, --help                Show help

Output Formats:
  text    Human-readable format (default)
  json    JSON format
  env     Environment variable format (KEY=value)

Revision Offset:
  The --offset parameter adds a number to the revision count.
  This is useful for large P4 repositories where you want to start
  from a smaller number for marketplace publishing.
  
  Example: If your P4 repository has 500300 revisions, use --offset -500000
  to get revision 300 instead.

Examples:
  # Get version info (text format)
  ./get-version-info.sh

  # Get version info as JSON
  ./get-version-info.sh --format json

  # Export as environment variables
  eval "\$(./get-version-info.sh --export)"
  echo \$PROJECT_REVISION_HASH_SHORT

  # Use revision offset (P4 example: 500300 - 500000 = 300)
  eval "\$(./get-version-info.sh --export --offset -500000)"
  echo \$PROJECT_REVISION  # Will show 300 instead of 500300

  # Detect VCS type only
  ./get-version-info.sh --detect-only

  # Get version info for specific repo
  ./get-version-info.sh /path/to/repo

Supported VCS Types:
  - git       Standard Git repository
  - git-p4    Git repository synced from Perforce
  - git-svn   Git repository synced from Subversion

Environment Variables (--export mode):
  Standard Git:
    PROJECT_REVISION_HASH_SHORT   Short commit hash
    PROJECT_REVISION_HASH         Full commit hash
    PROJECT_BRANCH                Current branch name
    PROJECT_REVISION              Commit count (with offset applied)
    PROJECT_REVISION_OFFSET       Offset value used
    PROJECT_TAG                   Latest tag
    PROJECT_VCS_TYPE              VCS type (git)

  Git-P4:
    PROJECT_REVISION_HASH_SHORT   Short commit hash
    PROJECT_REVISION_HASH         Full commit hash
    PROJECT_BRANCH                Git branch name
    PROJECT_REVISION              Git commit count (with offset applied)
    PROJECT_REVISION_OFFSET       Offset value used
    PROJECT_DEPOT                 Perforce depot name
    PROJECT_P4_STREAM             Perforce stream name
    PROJECT_P4_PROJECT            Perforce project name
    PROJECT_P4_CHANGE             Perforce change number
    PROJECT_VCS_TYPE              VCS type (git-p4)

  Git-SVN:
    PROJECT_REVISION_HASH_SHORT   Short commit hash
    PROJECT_REVISION_HASH         Full commit hash
    PROJECT_BRANCH                Git branch name
    PROJECT_REVISION              Git commit count (with offset applied)
    PROJECT_REVISION_OFFSET       Offset value used
    PROJECT_SVN_URL               SVN repository URL
    PROJECT_SVN_REVISION          SVN revision number
    PROJECT_SVN_BRANCH            SVN branch name
    PROJECT_VCS_TYPE              VCS type (git-svn)
EOF
}

log_error() {
  echo -e "${RED}[✗]${NC} $*" >&2
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Parse arguments
  local positional_args=()
  
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --format)
        OUTPUT_FORMAT="${2:-}"
        shift 2
        ;;
      --export)
        EXPORT_MODE=1
        shift
        ;;
      --offset)
        REVISION_OFFSET="${2:-0}"
        shift 2
        ;;
      --detect-only)
        DETECT_ONLY=1
        shift
        ;;
      -*)
        log_error "Unknown option: $1"
        usage
        exit 1
        ;;
      *)
        positional_args+=("$1")
        shift
        ;;
    esac
  done
  
  # Get repository path
  if [[ ${#positional_args[@]} -gt 0 ]]; then
    REPO_PATH="${positional_args[0]}"
  fi
  
  # Validate repository
  if [[ ! -d "$REPO_PATH/.git" ]]; then
    log_error "Not a git repository: $REPO_PATH"
    exit 1
  fi
  
  # Detect-only mode
  if [[ "$DETECT_ONLY" -eq 1 ]]; then
    detect_vcs_type "$REPO_PATH"
    exit 0
  fi
  
  # Export mode
  if [[ "$EXPORT_MODE" -eq 1 ]]; then
    export_version_vars "$REPO_PATH" "$REVISION_OFFSET"
    exit 0
  fi
  
  # Get version info
  case "$OUTPUT_FORMAT" in
    json)
      get_version_info "$REPO_PATH"
      ;;
    env)
      export_version_vars "$REPO_PATH" "$REVISION_OFFSET" | sed 's/^export //'
      ;;
    text)
      print_version_info "$REPO_PATH"
      ;;
    *)
      log_error "Unknown format: $OUTPUT_FORMAT"
      log_error "Valid formats: json, env, text"
      exit 1
      ;;
  esac
}

# Run main
main "$@"
