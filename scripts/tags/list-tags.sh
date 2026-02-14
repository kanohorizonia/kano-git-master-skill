#!/usr/bin/env bash
#
# list-tags.sh - List git tags
#
# Usage:
#   ./list-tags.sh [options]
#
# Options:
#   --pattern <pattern>    Filter tags by pattern (e.g., "v1.*")
#   --latest               Show only the latest tag
#   --latest-n <n>         Show latest N tags
#   --detailed             Show detailed tag information
#   --sort <name|date>     Sort by name or date (default: name)
#   --format <table|list>  Output format (default: table)
#   --help                 Show this help message
#
# Examples:
#   # List all tags
#   ./list-tags.sh
#
#   # List tags matching pattern
#   ./list-tags.sh --pattern "v1.*"
#
#   # Show latest tag
#   ./list-tags.sh --latest
#
#   # Show latest 5 tags
#   ./list-tags.sh --latest-n 5
#
#   # Show detailed information
#   ./list-tags.sh --detailed
#
#   # Sort by date (newest first)
#   ./list-tags.sh --sort date
#

set -euo pipefail

# Get script directory and project root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd -P)"

# Default options
PATTERN=""
LATEST_ONLY=0
LATEST_N=0
DETAILED=0
SORT_BY="name"
OUTPUT_FORMAT="table"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

List git tags for the repository.

Options:
  --pattern <pattern>    Filter tags by pattern (e.g., "v1.*", "*-beta")
  --latest               Show only the latest tag
  --latest-n <n>         Show latest N tags
  --detailed             Show detailed tag information (type, date, message)
  --sort <name|date>     Sort by name or date (default: name)
  --format <table|list>  Output format (default: table)
  --help                 Show this help message

Sort Options:
  name    Sort by tag name (semantic version aware)
  date    Sort by creation date (newest first)

Examples:
  # List all tags
  ./list-tags.sh

  # List tags matching pattern
  ./list-tags.sh --pattern "v1.*"
  ./list-tags.sh --pattern "*-beta"

  # Show latest tag
  ./list-tags.sh --latest

  # Show latest 5 tags
  ./list-tags.sh --latest-n 5

  # Show detailed information
  ./list-tags.sh --detailed

  # Sort by date (newest first)
  ./list-tags.sh --sort date

  # Simple list format
  ./list-tags.sh --format list
EOF
}

log_info() {
  echo -e "${BLUE}[ℹ]${NC} $*"
}

log_error() {
  echo -e "${RED}[✗]${NC} $*" >&2
}

# Get tags with sorting
get_tags() {
  local pattern="$1"
  local sort_by="$2"
  
  local sort_flag=""
  case "$sort_by" in
    name)
      sort_flag="--sort=-version:refname"
      ;;
    date)
      sort_flag="--sort=-creatordate"
      ;;
    *)
      log_error "Invalid sort option: $sort_by"
      return 1
      ;;
  esac
  
  if [[ -n "$pattern" ]]; then
    git -C "$PROJECT_ROOT" tag -l "$pattern" $sort_flag
  else
    git -C "$PROJECT_ROOT" tag -l $sort_flag
  fi
}

# Get tag type (lightweight or annotated)
get_tag_type() {
  local tag="$1"
  
  local tag_type
  tag_type=$(git -C "$PROJECT_ROOT" cat-file -t "$tag" 2>/dev/null || echo "unknown")
  
  if [[ "$tag_type" == "tag" ]]; then
    echo "annotated"
  elif [[ "$tag_type" == "commit" ]]; then
    echo "lightweight"
  else
    echo "unknown"
  fi
}

# Get tag date
get_tag_date() {
  local tag="$1"
  local tag_type
  tag_type=$(get_tag_type "$tag")
  
  if [[ "$tag_type" == "annotated" ]]; then
    # Annotated tag - get tagger date
    git -C "$PROJECT_ROOT" tag -l --format='%(creatordate:short)' "$tag"
  else
    # Lightweight tag - get commit date
    git -C "$PROJECT_ROOT" log -1 --format=%cd --date=short "$tag" 2>/dev/null || echo "unknown"
  fi
}

# Get tag message
get_tag_message() {
  local tag="$1"
  local tag_type
  tag_type=$(get_tag_type "$tag")
  
  if [[ "$tag_type" == "annotated" ]]; then
    git -C "$PROJECT_ROOT" tag -l --format='%(contents:subject)' "$tag"
  else
    echo "(lightweight tag)"
  fi
}

# Get commit hash for tag
get_tag_commit() {
  local tag="$1"
  git -C "$PROJECT_ROOT" rev-parse --short "$tag" 2>/dev/null || echo "unknown"
}

# List tags in table format
list_tags_table() {
  local tags=("$@")
  
  if [[ ${#tags[@]} -eq 0 ]]; then
    log_info "No tags found"
    return 0
  fi
  
  echo -e "${CYAN}Tag Name${NC}        ${CYAN}Type${NC}        ${CYAN}Date${NC}        ${CYAN}Commit${NC}"
  echo "────────────────────────────────────────────────────────────────"
  
  for tag in "${tags[@]}"; do
    local tag_type date_str commit_hash
    tag_type=$(get_tag_type "$tag")
    date_str=$(get_tag_date "$tag")
    commit_hash=$(get_tag_commit "$tag")
    
    # Format type with color
    local type_display
    if [[ "$tag_type" == "annotated" ]]; then
      type_display="${GREEN}annotated${NC}"
    else
      type_display="${YELLOW}lightweight${NC}"
    fi
    
    printf "%-20s %-15s %-12s %s\n" "$tag" "$(echo -e "$type_display")" "$date_str" "$commit_hash"
  done
}

# List tags in detailed format
list_tags_detailed() {
  local tags=("$@")
  
  if [[ ${#tags[@]} -eq 0 ]]; then
    log_info "No tags found"
    return 0
  fi
  
  for tag in "${tags[@]}"; do
    local tag_type date_str commit_hash message
    tag_type=$(get_tag_type "$tag")
    date_str=$(get_tag_date "$tag")
    commit_hash=$(get_tag_commit "$tag")
    message=$(get_tag_message "$tag")
    
    echo -e "${CYAN}Tag:${NC}     $tag"
    echo -e "${CYAN}Type:${NC}    $tag_type"
    echo -e "${CYAN}Date:${NC}    $date_str"
    echo -e "${CYAN}Commit:${NC}  $commit_hash"
    echo -e "${CYAN}Message:${NC} $message"
    echo ""
  done
}

# List tags in simple list format
list_tags_list() {
  local tags=("$@")
  
  if [[ ${#tags[@]} -eq 0 ]]; then
    log_info "No tags found"
    return 0
  fi
  
  for tag in "${tags[@]}"; do
    echo "$tag"
  done
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

main() {
  # Parse arguments
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -h|--help)
        usage
        exit 0
        ;;
      --pattern)
        PATTERN="${2:-}"
        if [[ -z "$PATTERN" ]]; then
          log_error "--pattern requires a value"
          exit 1
        fi
        shift 2
        ;;
      --latest)
        LATEST_ONLY=1
        LATEST_N=1
        shift
        ;;
      --latest-n)
        LATEST_N="${2:-0}"
        if [[ "$LATEST_N" -le 0 ]]; then
          log_error "--latest-n requires a positive number"
          exit 1
        fi
        shift 2
        ;;
      --detailed)
        DETAILED=1
        shift
        ;;
      --sort)
        SORT_BY="${2:-}"
        if [[ ! "$SORT_BY" =~ ^(name|date)$ ]]; then
          log_error "Invalid sort option: $SORT_BY"
          log_error "Must be one of: name, date"
          exit 1
        fi
        shift 2
        ;;
      --format)
        OUTPUT_FORMAT="${2:-}"
        if [[ ! "$OUTPUT_FORMAT" =~ ^(table|list)$ ]]; then
          log_error "Invalid format: $OUTPUT_FORMAT"
          log_error "Must be one of: table, list"
          exit 1
        fi
        shift 2
        ;;
      -*)
        log_error "Unknown option: $1"
        usage
        exit 1
        ;;
      *)
        log_error "Unexpected argument: $1"
        usage
        exit 1
        ;;
    esac
  done
  
  # Check if we're in a git repository
  if ! git -C "$PROJECT_ROOT" rev-parse --git-dir &>/dev/null; then
    log_error "Not a git repository: $PROJECT_ROOT"
    exit 1
  fi
  
  # Get tags
  local tags_output
  tags_output=$(get_tags "$PATTERN" "$SORT_BY")
  
  if [[ -z "$tags_output" ]]; then
    if [[ -n "$PATTERN" ]]; then
      log_info "No tags found matching pattern: $PATTERN"
    else
      log_info "No tags found"
    fi
    exit 0
  fi
  
  # Convert to array
  mapfile -t tags <<< "$tags_output"
  
  # Limit to latest N if requested
  if [[ "$LATEST_N" -gt 0 ]]; then
    tags=("${tags[@]:0:$LATEST_N}")
  fi
  
  # Display tags
  if [[ "$DETAILED" -eq 1 ]]; then
    list_tags_detailed "${tags[@]}"
  elif [[ "$OUTPUT_FORMAT" == "list" ]]; then
    list_tags_list "${tags[@]}"
  else
    list_tags_table "${tags[@]}"
  fi
  
  # Show count
  if [[ "$OUTPUT_FORMAT" != "list" ]]; then
    echo ""
    log_info "Total tags: ${#tags[@]}"
  fi
}

# Run main
main "$@"
