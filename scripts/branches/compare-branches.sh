#!/usr/bin/env bash
#
# compare-branches.sh - Compare commits between two branches
#
# Purpose:
#   Compare two branches to see which commits are in one but not the other.
#   Useful for understanding branch differences before merging or cherry-picking.
#
# Usage:
#   ./compare-branches.sh <base-branch> <compare-branch> [options]
#
# Arguments:
#   base-branch       Base branch (e.g., main)
#   compare-branch    Branch to compare (e.g., feature/new)
#
# Options:
#   --bidirectional   Show commits in both directions
#   --detailed        Show file changes for each commit
#   --oneline         Show only commit titles (one line per commit)
#   --format <table|json|markdown> Output format (default: table)
#   --output <file>   Save to file
#   --repo <path>     Repository path (default: current directory)
#   -h, --help        Show help
#
# Examples:
#   # Basic comparison
#   ./compare-branches.sh main feature/new
#
#   # Bidirectional comparison
#   ./compare-branches.sh main develop --bidirectional
#
#   # Detailed output with file changes
#   ./compare-branches.sh main feature/new --detailed
#
#   # JSON output
#   ./compare-branches.sh main feature/new --format json
#
#   # Save to markdown file
#   ./compare-branches.sh main feature/new --format markdown --output diff.md
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

#------------------------------------------------------------------------------
# Configuration
#------------------------------------------------------------------------------

BASE_BRANCH=""
COMPARE_BRANCH=""
BIDIRECTIONAL=0
DETAILED=0
ONELINE=0
OUTPUT_FORMAT="table"
OUTPUT_FILE=""
REPO_PATH="."

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") <base-branch> <compare-branch> [options]

Compare commits between two branches.

Arguments:
  base-branch       Base branch (e.g., main)
  compare-branch    Branch to compare (e.g., feature/new)

Options:
  --bidirectional   Show commits in both directions
  --detailed        Show file changes for each commit
  --oneline         Show only commit titles
  --format <table|json|markdown> Output format (default: table)
  --output <file>   Save to file
  --repo <path>     Repository path (default: current directory)
  -h, --help        Show help

Examples:
  # Basic comparison
  ./compare-branches.sh main feature/new

  # Bidirectional comparison
  ./compare-branches.sh main develop --bidirectional

  # Detailed output
  ./compare-branches.sh main feature/new --detailed

  # JSON output
  ./compare-branches.sh main feature/new --format json

  # Save to markdown
  ./compare-branches.sh main feature/new --format markdown --output diff.md
EOF
}

# Get commits in branch A but not in branch B
get_commits_diff() {
  local base="$1"
  local compare="$2"
  local repo="$3"
  
  if [[ "$ONELINE" -eq 1 ]]; then
    (cd "$repo" && git log --oneline "$base..$compare" 2>/dev/null)
  elif [[ "$DETAILED" -eq 1 ]]; then
    (cd "$repo" && git log --stat "$base..$compare" 2>/dev/null)
  else
    (cd "$repo" && git log --format="%H|%an|%ae|%ad|%s" --date=short "$base..$compare" 2>/dev/null)
  fi
}

# Count commits
count_commits() {
  local base="$1"
  local compare="$2"
  local repo="$3"
  
  (cd "$repo" && git rev-list --count "$base..$compare" 2>/dev/null || echo "0")
}

# Get file statistics
get_file_stats() {
  local base="$1"
  local compare="$2"
  local repo="$3"
  
  (cd "$repo" && git diff --shortstat "$base...$compare" 2>/dev/null)
}

# Format output as table
format_as_table() {
  local base="$1"
  local compare="$2"
  local ahead_commits="$3"
  local behind_commits="$4"
  local file_stats="$5"
  
  echo "Branch Comparison: $base...$compare"
  echo ""
  
  if [[ "$ahead_commits" -gt 0 ]]; then
    echo "Commits in $compare but not in $base ($ahead_commits):"
    while IFS='|' read -r hash author email date subject; do
      if [[ -n "$hash" ]]; then
        echo "  ${hash:0:7} $subject"
        if [[ "$DETAILED" -eq 1 ]]; then
          echo "           Author: $author <$email>"
          echo "           Date: $date"
        fi
      fi
    done <<< "$(get_commits_diff "$base" "$compare" "$REPO_PATH")"
    echo ""
  else
    echo "No commits in $compare that are not in $base"
    echo ""
  fi
  
  if [[ "$BIDIRECTIONAL" -eq 1 ]] && [[ "$behind_commits" -gt 0 ]]; then
    echo "Commits in $base but not in $compare ($behind_commits):"
    while IFS='|' read -r hash author email date subject; do
      if [[ -n "$hash" ]]; then
        echo "  ${hash:0:7} $subject"
        if [[ "$DETAILED" -eq 1 ]]; then
          echo "           Author: $author <$email>"
          echo "           Date: $date"
        fi
      fi
    done <<< "$(get_commits_diff "$compare" "$base" "$REPO_PATH")"
    echo ""
  fi
  
  echo "Summary:"
  echo "  Ahead: $ahead_commits commits"
  if [[ "$BIDIRECTIONAL" -eq 1 ]]; then
    echo "  Behind: $behind_commits commits"
  fi
  if [[ -n "$file_stats" ]]; then
    echo "  $file_stats"
  fi
}

# Format output as JSON
format_as_json() {
  local base="$1"
  local compare="$2"
  local ahead_commits="$3"
  local behind_commits="$4"
  local file_stats="$5"
  
  local json="{"
  json+="\"base_branch\":\"$base\","
  json+="\"compare_branch\":\"$compare\","
  json+="\"ahead\":$ahead_commits,"
  json+="\"behind\":$behind_commits,"
  json+="\"commits_ahead\":["
  
  local first=1
  while IFS='|' read -r hash author email date subject; do
    if [[ -n "$hash" ]]; then
      if [[ $first -eq 0 ]]; then
        json+=","
      fi
      first=0
      
      # Escape quotes in subject
      local escaped_subject="${subject//\"/\\\"}"
      local escaped_author="${author//\"/\\\"}"
      
      json+="{"
      json+="\"hash\":\"$hash\","
      json+="\"short_hash\":\"${hash:0:7}\","
      json+="\"author\":\"$escaped_author\","
      json+="\"email\":\"$email\","
      json+="\"date\":\"$date\","
      json+="\"subject\":\"$escaped_subject\""
      json+="}"
    fi
  done <<< "$(get_commits_diff "$base" "$compare" "$REPO_PATH")"
  
  json+="]"
  
  if [[ "$BIDIRECTIONAL" -eq 1 ]]; then
    json+=",\"commits_behind\":["
    
    first=1
    while IFS='|' read -r hash author email date subject; do
      if [[ -n "$hash" ]]; then
        if [[ $first -eq 0 ]]; then
          json+=","
        fi
        first=0
        
        local escaped_subject="${subject//\"/\\\"}"
        local escaped_author="${author//\"/\\\"}"
        
        json+="{"
        json+="\"hash\":\"$hash\","
        json+="\"short_hash\":\"${hash:0:7}\","
        json+="\"author\":\"$escaped_author\","
        json+="\"email\":\"$email\","
        json+="\"date\":\"$date\","
        json+="\"subject\":\"$escaped_subject\""
        json+="}"
      fi
    done <<< "$(get_commits_diff "$compare" "$base" "$REPO_PATH")"
    
    json+="]"
  fi
  
  json+="}"
  
  echo "$json"
}

# Format output as markdown
format_as_markdown() {
  local base="$1"
  local compare="$2"
  local ahead_commits="$3"
  local behind_commits="$4"
  local file_stats="$5"
  
  echo "# Branch Comparison: $base...$compare"
  echo ""
  echo "**Generated:** $(date '+%Y-%m-%d %H:%M:%S')"
  echo ""
  
  echo "## Summary"
  echo ""
  echo "- **Ahead**: $ahead_commits commits"
  if [[ "$BIDIRECTIONAL" -eq 1 ]]; then
    echo "- **Behind**: $behind_commits commits"
  fi
  if [[ -n "$file_stats" ]]; then
    echo "- **Changes**: $file_stats"
  fi
  echo ""
  
  if [[ "$ahead_commits" -gt 0 ]]; then
    echo "## Commits in \`$compare\` but not in \`$base\`"
    echo ""
    while IFS='|' read -r hash author email date subject; do
      if [[ -n "$hash" ]]; then
        echo "### \`${hash:0:7}\` $subject"
        echo ""
        echo "- **Author**: $author <$email>"
        echo "- **Date**: $date"
        echo ""
      fi
    done <<< "$(get_commits_diff "$base" "$compare" "$REPO_PATH")"
  fi
  
  if [[ "$BIDIRECTIONAL" -eq 1 ]] && [[ "$behind_commits" -gt 0 ]]; then
    echo "## Commits in \`$base\` but not in \`$compare\`"
    echo ""
    while IFS='|' read -r hash author email date subject; do
      if [[ -n "$hash" ]]; then
        echo "### \`${hash:0:7}\` $subject"
        echo ""
        echo "- **Author**: $author <$email>"
        echo "- **Date**: $date"
        echo ""
      fi
    done <<< "$(get_commits_diff "$compare" "$base" "$REPO_PATH")"
  fi
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
      --bidirectional)
        BIDIRECTIONAL=1
        shift
        ;;
      --detailed)
        DETAILED=1
        shift
        ;;
      --oneline)
        ONELINE=1
        shift
        ;;
      --format)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --format requires an argument"
          usage
          exit 1
        fi
        OUTPUT_FORMAT="$2"
        shift 2
        ;;
      --output)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --output requires an argument"
          usage
          exit 1
        fi
        OUTPUT_FILE="$2"
        shift 2
        ;;
      --repo)
        if [[ -z "${2:-}" ]]; then
          gith_error "Option --repo requires an argument"
          usage
          exit 1
        fi
        REPO_PATH="$2"
        shift 2
        ;;
      -*)
        gith_error "Unknown option: $1"
        usage
        exit 1
        ;;
      *)
        positional_args+=("$1")
        shift
        ;;
    esac
  done
  
  # Get branches from positional arguments
  if [[ ${#positional_args[@]} -lt 2 ]]; then
    gith_error "Both base-branch and compare-branch are required"
    usage
    exit 1
  fi
  
  BASE_BRANCH="${positional_args[0]}"
  COMPARE_BRANCH="${positional_args[1]}"
  
  # Validate repository
  if ! gith_is_git_repo "$REPO_PATH"; then
    gith_error "Not a git repository: $REPO_PATH"
    exit 1
  fi
  
  # Validate branches exist
  if ! (cd "$REPO_PATH" && git rev-parse --verify "$BASE_BRANCH" >/dev/null 2>&1); then
    gith_error "Branch does not exist: $BASE_BRANCH"
    exit 1
  fi
  
  if ! (cd "$REPO_PATH" && git rev-parse --verify "$COMPARE_BRANCH" >/dev/null 2>&1); then
    gith_error "Branch does not exist: $COMPARE_BRANCH"
    exit 1
  fi
  
  # Validate output format
  if [[ "$OUTPUT_FORMAT" != "table" ]] && [[ "$OUTPUT_FORMAT" != "json" ]] && [[ "$OUTPUT_FORMAT" != "markdown" ]]; then
    gith_error "Invalid output format: $OUTPUT_FORMAT (must be 'table', 'json', or 'markdown')"
    exit 1
  fi
  
  # Get commit counts
  local ahead_commits behind_commits
  ahead_commits="$(count_commits "$BASE_BRANCH" "$COMPARE_BRANCH" "$REPO_PATH")"
  behind_commits=0
  
  if [[ "$BIDIRECTIONAL" -eq 1 ]]; then
    behind_commits="$(count_commits "$COMPARE_BRANCH" "$BASE_BRANCH" "$REPO_PATH")"
  fi
  
  # Get file statistics
  local file_stats
  file_stats="$(get_file_stats "$BASE_BRANCH" "$COMPARE_BRANCH" "$REPO_PATH")"
  
  # Generate output
  local output=""
  if [[ "$OUTPUT_FORMAT" == "table" ]]; then
    output="$(format_as_table "$BASE_BRANCH" "$COMPARE_BRANCH" "$ahead_commits" "$behind_commits" "$file_stats")"
  elif [[ "$OUTPUT_FORMAT" == "json" ]]; then
    output="$(format_as_json "$BASE_BRANCH" "$COMPARE_BRANCH" "$ahead_commits" "$behind_commits" "$file_stats")"
  elif [[ "$OUTPUT_FORMAT" == "markdown" ]]; then
    output="$(format_as_markdown "$BASE_BRANCH" "$COMPARE_BRANCH" "$ahead_commits" "$behind_commits" "$file_stats")"
  fi
  
  # Output to file or stdout
  if [[ -n "$OUTPUT_FILE" ]]; then
    echo "$output" > "$OUTPUT_FILE"
    gith_log "INFO" "Comparison saved to: $OUTPUT_FILE"
  else
    echo "$output"
  fi
  
  exit 0
}

# Run main function
main "$@"
