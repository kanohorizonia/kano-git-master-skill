#!/usr/bin/env bash
#
# cherry-pick-batch.sh - Batch cherry-pick commits from a structured file
#
# Purpose:
#   Cherry-pick multiple commits from a file containing commit hashes and metadata.
#   Supports both JSON and simple text formats for easy tracking of what's being picked.
#
# Usage:
#   ./cherry-pick-batch.sh <input-file> [options]
#
# Arguments:
#   input-file        File containing commits to cherry-pick (JSON or TXT format)
#
# Options:
#   --continue        Continue after resolving conflicts
#   --abort           Abort cherry-pick operation
#   --skip            Skip current commit and continue
#   --dry-run         Preview commits without applying
#   --repo <path>     Repository path (default: current directory)
#   -h, --help        Show help
#
# File Formats:
#
#   JSON format (recommended):
#   {
#     "commits": [
#       {
#         "hash": "abc123",
#         "title": "feat: Add new feature",
#         "author": "John Doe",
#         "date": "2024-01-15"
#       },
#       {
#         "hash": "def456",
#         "title": "fix: Bug fix"
#       }
#     ]
#   }
#
#   Text format (simple):
#   abc123 feat: Add new feature
#   def456 fix: Bug fix
#   # Comments are ignored
#   
#   Or just hashes (one per line):
#   abc123
#   def456
#
# Examples:
#   # Cherry-pick from JSON file
#   ./cherry-pick-batch.sh commits.json
#
#   # Cherry-pick from text file
#   ./cherry-pick-batch.sh commits.txt
#
#   # Preview without applying
#   ./cherry-pick-batch.sh commits.json --dry-run
#
#   # Continue after resolving conflicts
#   ./cherry-pick-batch.sh commits.json --continue
#
#   # Abort operation
#   ./cherry-pick-batch.sh commits.json --abort
#

set -euo pipefail

# Get script directory and source helpers
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

#------------------------------------------------------------------------------
# Configuration
#------------------------------------------------------------------------------

INPUT_FILE=""
REPO_PATH="."
DRY_RUN=0
CONTINUE_MODE=0
ABORT_MODE=0
SKIP_MODE=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat << EOF
Usage: $(basename "$0") <input-file> [options]

Batch cherry-pick commits from a structured file.

Arguments:
  input-file        File containing commits (JSON or TXT format)

Options:
  --continue        Continue after resolving conflicts
  --abort           Abort cherry-pick operation
  --skip            Skip current commit and continue
  --dry-run         Preview commits without applying
  --repo <path>     Repository path (default: current directory)
  -h, --help        Show help

File Formats:

JSON format (recommended):
{
  "commits": [
    {
      "hash": "abc123",
      "title": "feat: Add new feature",
      "author": "John Doe",
      "date": "2024-01-15"
    }
  ]
}

Text format (simple):
abc123 feat: Add new feature
def456 fix: Bug fix
# Comments are ignored

Or just hashes:
abc123
def456

Examples:
  # Cherry-pick from JSON
  ./cherry-pick-batch.sh commits.json

  # Preview
  ./cherry-pick-batch.sh commits.json --dry-run

  # Continue after conflicts
  ./cherry-pick-batch.sh commits.json --continue

  # Abort
  ./cherry-pick-batch.sh commits.json --abort
EOF
}

# Parse JSON file
parse_json_file() {
  local file="$1"
  
  # Check if jq is available
  if ! command -v jq >/dev/null 2>&1; then
    gith_error "jq is required for JSON parsing. Install: apt-get install jq / brew install jq"
    return 1
  fi
  
  # Extract commits array
  if ! jq -e '.commits' "$file" >/dev/null 2>&1; then
    gith_error "Invalid JSON format: missing 'commits' array"
    return 1
  fi
  
  # Output: hash|title|author|date (one per line)
  jq -r '.commits[] | "\(.hash)|\(.title // "")|\(.author // "")|\(.date // "")"' "$file"
}

# Parse text file
parse_text_file() {
  local file="$1"
  
  while IFS= read -r line; do
    # Skip empty lines and comments
    if [[ -z "$line" ]] || [[ "$line" =~ ^[[:space:]]*# ]]; then
      continue
    fi
    
    # Extract hash (first word)
    local hash
    hash="$(echo "$line" | awk '{print $1}')"
    
    # Extract title (rest of line)
    local title
    title="$(echo "$line" | cut -d' ' -f2- 2>/dev/null || echo "")"
    
    if [[ -n "$hash" ]]; then
      echo "$hash|$title||"
    fi
  done < "$file"
}

# Detect file format and parse
parse_input_file() {
  local file="$1"
  
  if [[ ! -f "$file" ]]; then
    gith_error "File not found: $file"
    return 1
  fi
  
  # Detect format by content
  if head -n 1 "$file" | grep -q '^[[:space:]]*{'; then
    # JSON format
    parse_json_file "$file"
  else
    # Text format
    parse_text_file "$file"
  fi
}

# Validate commit hash exists
validate_commit() {
  local hash="$1"
  local repo="$2"
  
  (cd "$repo" && git rev-parse --verify "$hash^{commit}" >/dev/null 2>&1)
}

# Get commit info
get_commit_info() {
  local hash="$1"
  local repo="$2"
  
  (cd "$repo" && git log -1 --format="%h|%s|%an|%ad" --date=short "$hash" 2>/dev/null)
}

# Cherry-pick single commit
cherry_pick_commit() {
  local hash="$1"
  local title="$2"
  local repo="$3"
  
  gith_log "INFO" "Cherry-picking: $hash ${title:+- $title}"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    gith_log "INFO" "[DRY-RUN] Would cherry-pick: $hash"
    return 0
  fi
  
  if ! (cd "$repo" && git cherry-pick "$hash" 2>&1); then
    gith_error "Cherry-pick failed for: $hash"
    gith_log "INFO" "Resolve conflicts and run: $(basename "$0") $INPUT_FILE --continue"
    gith_log "INFO" "Or skip: $(basename "$0") $INPUT_FILE --skip"
    gith_log "INFO" "Or abort: $(basename "$0") $INPUT_FILE --abort"
    return 1
  fi
  
  return 0
}

# Check if cherry-pick is in progress
is_cherry_pick_in_progress() {
  local repo="$1"
  
  [[ -d "$repo/.git/sequencer" ]] || [[ -f "$repo/.git/CHERRY_PICK_HEAD" ]]
}

# Continue cherry-pick
continue_cherry_pick() {
  local repo="$1"
  
  if ! is_cherry_pick_in_progress "$repo"; then
    gith_error "No cherry-pick in progress"
    return 1
  fi
  
  gith_log "INFO" "Continuing cherry-pick..."
  
  if ! (cd "$repo" && git cherry-pick --continue 2>&1); then
    gith_error "Failed to continue cherry-pick"
    return 1
  fi
  
  gith_log "INFO" "Cherry-pick continued successfully"
  return 0
}

# Abort cherry-pick
abort_cherry_pick() {
  local repo="$1"
  
  if ! is_cherry_pick_in_progress "$repo"; then
    gith_error "No cherry-pick in progress"
    return 1
  fi
  
  gith_log "INFO" "Aborting cherry-pick..."
  
  if ! (cd "$repo" && git cherry-pick --abort 2>&1); then
    gith_error "Failed to abort cherry-pick"
    return 1
  fi
  
  gith_log "INFO" "Cherry-pick aborted"
  return 0
}

# Skip current commit
skip_cherry_pick() {
  local repo="$1"
  
  if ! is_cherry_pick_in_progress "$repo"; then
    gith_error "No cherry-pick in progress"
    return 1
  fi
  
  gith_log "INFO" "Skipping current commit..."
  
  if ! (cd "$repo" && git cherry-pick --skip 2>&1); then
    gith_error "Failed to skip commit"
    return 1
  fi
  
  gith_log "INFO" "Commit skipped"
  return 0
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
      --continue)
        CONTINUE_MODE=1
        shift
        ;;
      --abort)
        ABORT_MODE=1
        shift
        ;;
      --skip)
        SKIP_MODE=1
        shift
        ;;
      --dry-run)
        DRY_RUN=1
        shift
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
  
  # Get input file from positional arguments
  if [[ ${#positional_args[@]} -lt 1 ]]; then
    gith_error "Input file is required"
    usage
    exit 1
  fi
  
  INPUT_FILE="${positional_args[0]}"
  
  # Validate repository
  if ! gith_is_git_repo "$REPO_PATH"; then
    gith_error "Not a git repository: $REPO_PATH"
    exit 1
  fi
  
  # Handle special modes
  if [[ "$CONTINUE_MODE" -eq 1 ]]; then
    continue_cherry_pick "$REPO_PATH"
    exit $?
  fi
  
  if [[ "$ABORT_MODE" -eq 1 ]]; then
    abort_cherry_pick "$REPO_PATH"
    exit $?
  fi
  
  if [[ "$SKIP_MODE" -eq 1 ]]; then
    skip_cherry_pick "$REPO_PATH"
    exit $?
  fi
  
  # Check if cherry-pick already in progress
  if is_cherry_pick_in_progress "$REPO_PATH"; then
    gith_error "Cherry-pick already in progress"
    gith_log "INFO" "Use --continue, --skip, or --abort"
    exit 1
  fi
  
  # Parse input file
  gith_log "INFO" "Parsing input file: $INPUT_FILE"
  
  local commits
  if ! commits="$(parse_input_file "$INPUT_FILE")"; then
    exit 1
  fi
  
  if [[ -z "$commits" ]]; then
    gith_error "No commits found in input file"
    exit 1
  fi
  
  # Count commits
  local commit_count
  commit_count="$(echo "$commits" | wc -l)"
  gith_log "INFO" "Found $commit_count commits to cherry-pick"
  
  # Validate all commits first
  gith_log "INFO" "Validating commits..."
  
  local validation_failed=0
  while IFS='|' read -r hash title author date; do
    if [[ -z "$hash" ]]; then
      continue
    fi
    
    if ! validate_commit "$hash" "$REPO_PATH"; then
      gith_error "Invalid commit hash: $hash"
      validation_failed=1
    else
      # Get actual commit info if not provided
      if [[ -z "$title" ]]; then
        local info
        info="$(get_commit_info "$hash" "$REPO_PATH")"
        title="$(echo "$info" | cut -d'|' -f2)"
      fi
      
      if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "  ✓ $hash - $title"
      fi
    fi
  done <<< "$commits"
  
  if [[ "$validation_failed" -eq 1 ]]; then
    gith_error "Validation failed. Please check commit hashes."
    exit 1
  fi
  
  gith_log "INFO" "All commits validated successfully"
  
  if [[ "$DRY_RUN" -eq 1 ]]; then
    gith_log "INFO" "[DRY-RUN] Would cherry-pick $commit_count commits"
    exit 0
  fi
  
  # Cherry-pick commits
  gith_log "INFO" "Starting cherry-pick operation..."
  
  local success_count=0
  local failed_count=0
  
  while IFS='|' read -r hash title author date; do
    if [[ -z "$hash" ]]; then
      continue
    fi
    
    if cherry_pick_commit "$hash" "$title" "$REPO_PATH"; then
      ((success_count+=1))
    else
      ((failed_count+=1))
      # Stop on first failure
      break
    fi
  done <<< "$commits"
  
  # Summary
  echo ""
  gith_log "INFO" "Cherry-pick summary:"
  gith_log "INFO" "  Success: $success_count"
  gith_log "INFO" "  Failed: $failed_count"
  
  if [[ "$failed_count" -gt 0 ]]; then
    exit 1
  fi
  
  gith_log "INFO" "All commits cherry-picked successfully"
  exit 0
}

# Run main function
main "$@"
