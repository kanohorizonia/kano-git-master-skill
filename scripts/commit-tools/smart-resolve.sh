#!/usr/bin/env bash
#
# smart-resolve.sh - AI-powered conflict resolution
#
# Purpose:
#   Automatically resolve Git conflicts using AI
#
# Usage:
#   ./smart-resolve.sh --provider <name> --model <name> [options]
#
# Required Options:
#   --provider <name>           AI provider (opencode, codex, copilot)
#   --model <name>              AI model name
#
# Optional:
#   --interactive               Review each resolution before applying
#   --auto                      Auto-apply all resolutions (default)
#   --file <path>               Only resolve specific file
#   --dry-run                   Show resolutions without applying
#   -h, --help                  Show help
#
# Examples:
#   # Auto-resolve all conflicts
#   ./smart-resolve.sh --provider copilot --model gpt-4o
#
#   # Interactive mode (review each)
#   ./smart-resolve.sh --provider copilot --model gpt-4o --interactive
#
#   # Resolve specific file
#   ./smart-resolve.sh --provider copilot --model gpt-4o --file src/main.ts
#
#   # Dry run
#   ./smart-resolve.sh --provider copilot --model gpt-4o --dry-run
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load libraries
source "$SCRIPT_DIR/lib/ai-providers.sh"
source "$SCRIPT_DIR/lib/git-helpers.sh"
source "$SCRIPT_DIR/lib/conflict-parser.sh"

# Configuration
AI_PROVIDER=""
AI_MODEL=""
INTERACTIVE=0
DRY_RUN=0
TARGET_FILE=""
REPO="."

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-resolve.sh --provider <name> --model <name> [options]

AI-powered Git conflict resolution.

Required Options:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Optional:
  --interactive               Review each resolution before applying
  --auto                      Auto-apply all resolutions (default)
  --file <path>               Only resolve specific file
  --dry-run                   Show resolutions without applying
  -h, --help                  Show help

Examples:
  # Auto-resolve all conflicts
  ./smart-resolve.sh --provider copilot --model gpt-4o

  # Interactive mode (review each)
  ./smart-resolve.sh --provider copilot --model gpt-4o --interactive

  # Resolve specific file
  ./smart-resolve.sh --provider copilot --model gpt-4o --file src/main.ts

  # Dry run
  ./smart-resolve.sh --provider copilot --model gpt-4o --dry-run

Workflow:
  1. Detect conflicted files
  2. For each file:
     a. Extract conflict markers
     b. Build AI prompt with context
     c. Get AI resolution
     d. Apply or review resolution
  3. Stage resolved files
  4. Continue merge/rebase operation

Safety:
  - Creates .conflict-backup files
  - Interactive mode for review
  - Dry-run mode available
  - Validates resolution before applying
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --provider)
      AI_PROVIDER="${2:-}"
      shift 2
      ;;
    --model)
      AI_MODEL="${2:-}"
      shift 2
      ;;
    --interactive)
      INTERACTIVE=1
      shift
      ;;
    --auto)
      INTERACTIVE=0
      shift
      ;;
    --file)
      TARGET_FILE="${2:-}"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# Validate required parameters
if [[ -z "$AI_PROVIDER" || -z "$AI_MODEL" ]]; then
  echo "ERROR: --provider and --model are required" >&2
  usage >&2
  exit 1
fi

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

echo "=== Smart Conflict Resolution ==="
echo ""

# Validate repository
if ! validate_repo "$REPO"; then
  exit 1
fi

# Check for conflicts
if ! has_conflicts "$REPO"; then
  echo "No conflicts detected"
  exit 0
fi

# Get operation in progress
operation="$(get_operation_in_progress "$REPO")"
if [[ -n "$operation" ]]; then
  echo "Detected $operation in progress"
fi

# Get conflicted files
declare -a files=()
if [[ -n "$TARGET_FILE" ]]; then
  if [[ -f "$REPO/$TARGET_FILE" ]] && has_conflict_markers "$REPO/$TARGET_FILE"; then
    files=("$TARGET_FILE")
  else
    echo "ERROR: File not found or has no conflicts: $TARGET_FILE" >&2
    exit 1
  fi
else
  while IFS= read -r file; do
    files+=("$file")
  done < <(get_conflicted_files "$REPO")
fi

echo "Found ${#files[@]} conflicted file(s)"
echo ""

# Show conflict statistics
get_conflict_stats "$REPO"
echo ""

# Process each file
resolved_count=0
failed_count=0

for file in "${files[@]}"; do
  echo "=== Processing: $file ==="

  conflict_count="$(count_conflict_markers "$REPO/$file")"
  echo "Conflicts: $conflict_count"

  # Build AI prompt
  prompt="$(build_conflict_prompt "$REPO/$file" "$REPO")"

  # Get AI resolution
  echo "Requesting AI resolution..."
  resolution="$(ai_generate_message "$AI_PROVIDER" "$AI_MODEL" "$prompt" || true)"

  if [[ -z "$resolution" ]]; then
    echo "ERROR: Failed to get AI resolution" >&2
    ((failed_count++)) || true
    continue
  fi

  # Extract resolution content (remove explanation)
  resolution_content="$(echo "$resolution" | sed -n '/^RESOLUTION:/,/^EXPLANATION:/p' | sed '1d;$d')"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would apply resolution:"
    echo "$resolution_content" | head -n 10
    echo "..."
    ((resolved_count++)) || true
    continue
  fi

  # Interactive mode
  if [[ "$INTERACTIVE" -eq 1 ]]; then
    echo ""
    echo "Proposed resolution:"
    echo "$resolution_content" | head -n 20
    echo ""
    read -p "Apply this resolution? [y/N] " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
      echo "Skipped"
      continue
    fi
  fi

  # Apply resolution
  backup="$(apply_resolution "$REPO/$file" "$resolution_content")"
  echo "Applied resolution (backup: $backup)"

  # Stage resolved file
  git -C "$REPO" add "$file" 2>/dev/null || true

  ((resolved_count++)) || true
  echo ""
done

echo "=== Resolution Complete ==="
echo "Resolved: $resolved_count"
echo "Failed: $failed_count"
echo ""

if [[ "$resolved_count" -gt 0 ]] && [[ "$DRY_RUN" -eq 0 ]]; then
  echo "Next steps:"
  if [[ "$operation" == "rebase" ]]; then
    echo "  git rebase --continue"
  elif [[ "$operation" == "merge" ]]; then
    echo "  git commit"
  elif [[ "$operation" == "cherry-pick" ]]; then
    echo "  git cherry-pick --continue"
  else
    echo "  git commit"
  fi
fi
