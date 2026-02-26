#!/usr/bin/env bash
#
# show-version.sh - Display Git Master Skill version information
#
# Usage:
#   ./show-version.sh [--short|--full]
#
# Options:
#   --short    Show version number only
#   --full     Show detailed version information (default)
#   --help     Show this help message
#
# Examples:
#   ./show-version.sh
#   ./show-version.sh --short
#   ./show-version.sh --full
#

set -euo pipefail

# Get script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd -P)"
VERSION_FILE="$PROJECT_ROOT/VERSION"

# Default mode
MODE="full"

# Colors
BLUE='\033[0;34m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") [--short|--full]

Display Git Master Skill version information.

Options:
  --short    Show version number only
  --full     Show detailed version information (default)
  --help     Show this help message

Examples:
  # Show full version info
  ./show-version.sh

  # Show version number only
  ./show-version.sh --short

  # Use in scripts
  VERSION=\$(./show-version.sh --short)
  echo "Using Git Master Skill v\$VERSION"
EOF
}

get_version() {
  if [[ -f "$VERSION_FILE" ]]; then
    cat "$VERSION_FILE" | tr -d '\n\r'
  else
    echo "unknown"
  fi
}

get_git_info() {
  if git -C "$PROJECT_ROOT" rev-parse --git-dir &>/dev/null; then
    local commit_hash
    local commit_date
    local branch
    
    commit_hash=$(git -C "$PROJECT_ROOT" rev-parse --short HEAD 2>/dev/null || echo "unknown")
    commit_date=$(git -C "$PROJECT_ROOT" log -1 --format=%cd --date=short 2>/dev/null || echo "unknown")
    branch=$(git -C "$PROJECT_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
    
    echo "$commit_hash|$commit_date|$branch"
  else
    echo "unknown|unknown|unknown"
  fi
}

show_short() {
  get_version
}

show_full() {
  local version
  local git_info
  local commit_hash
  local commit_date
  local branch
  
  version=$(get_version)
  git_info=$(get_git_info)
  
  IFS='|' read -r commit_hash commit_date branch <<< "$git_info"
  
  echo -e "${GREEN}Git Master Skill${NC}"
  echo ""
  echo -e "${BLUE}Version:${NC}     $version"
  
  if [[ "$commit_hash" != "unknown" ]]; then
    echo -e "${BLUE}Commit:${NC}      $commit_hash"
    echo -e "${BLUE}Date:${NC}        $commit_date"
    echo -e "${BLUE}Branch:${NC}      $branch"
  fi
  
  echo ""
  
  # Show status based on version
  if [[ "$version" == *"beta"* ]]; then
    echo -e "${YELLOW}Status:${NC}      Beta Release"
    echo -e "${YELLOW}Note:${NC}        All features functional, feedback welcome!"
  elif [[ "$version" == *"alpha"* ]]; then
    echo -e "${YELLOW}Status:${NC}      Alpha Release"
    echo -e "${YELLOW}Note:${NC}        Early development, may be unstable"
  elif [[ "$version" == *"rc"* ]]; then
    echo -e "${YELLOW}Status:${NC}      Release Candidate"
    echo -e "${YELLOW}Note:${NC}        Final testing before stable release"
  else
    echo -e "${GREEN}Status:${NC}      Stable Release"
  fi
  
  echo ""
  echo "Documentation: $PROJECT_ROOT/docs/README.md"
  echo "Changelog:     $PROJECT_ROOT/docs/status/changelog.md"
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --short)
        MODE="short"
        shift
        ;;
      --full)
        MODE="full"
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
  
  # Check if VERSION file exists
  if [[ ! -f "$VERSION_FILE" ]]; then
    echo "Error: VERSION file not found: $VERSION_FILE" >&2
    exit 1
  fi
  
  # Show version
  case "$MODE" in
    short)
      show_short
      ;;
    full)
      show_full
      ;;
  esac
}

# Run main
main "$@"
