#!/usr/bin/env bash
#
# smart-commit-push.sh - AI-powered commit, rebase, and push workflow
#
# Purpose:
#   Complete workflow: commit → fetch → rebase → resolve conflicts (AI) → push
#
# Usage:
#   ./smart-commit-push.sh --provider <name> --model <name> [options]
#
# Required Options:
#   --provider <name>           AI provider (opencode, codex, copilot)
#   --model <name>              AI model name
#
# Optional:
#   --repos <paths>             Only process specific repos (comma-separated)
#   --rules <text>              Custom commit rules (inline text)
#   --rules-file <path>         Custom commit rules (from file)
#   --no-ai-review              Disable AI safety review
#   --dry-run                   Show what would be done without doing it
#   -h, --help                  Show help
#
# Examples:
#   # Full workflow with default settings
#   ./smart-commit-push.sh --provider copilot --model gpt-4o
#
#   # Only process root and specific submodule
#   ./smart-commit-push.sh --provider copilot --model gpt-4o --repos ".,submodules/my-lib"
#
#   # Dry run to see what would happen
#   ./smart-commit-push.sh --provider copilot --model gpt-4o --dry-run
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Configuration
DRY_RUN=0
SMART_COMMIT_ARGS=()

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-commit-push.sh --provider <name> --model <name> [options]

Complete AI-powered workflow: commit → fetch → rebase → resolve conflicts → push

Required Options:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Optional:
  --repos <paths>             Only process specific repos (comma-separated)
  --rules <text>              Custom commit rules (inline text)
  --rules-file <path>         Custom commit rules (from file)
  --no-ai-review              Disable AI safety review
  --dry-run                   Show what would be done without doing it
  -h, --help                  Show help

Examples:
  # Full workflow with default settings
  ./smart-commit-push.sh --provider copilot --model gpt-4o

  # Only process root and specific submodule
  ./smart-commit-push.sh --provider copilot --model gpt-4o --repos ".,submodules/my-lib"

  # Dry run to see what would happen
  ./smart-commit-push.sh --provider copilot --model gpt-4o --dry-run

Workflow Steps:
  1. Commit changes (using smart-commit.sh)
  2. Fetch latest from remote
  3. Rebase onto remote branch
  4. If conflicts: Use AI to resolve them
  5. Push with --force-with-lease

Safety:
  - Uses --force-with-lease (safe force push)
  - AI reviews conflict resolution
  - Dry run mode available
  - Aborts on unresolvable conflicts
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      # Pass through to smart-commit.sh
      SMART_COMMIT_ARGS+=("$1")
      shift
      ;;
  esac
done

# Validate that we have required args
if [[ "${#SMART_COMMIT_ARGS[@]}" -eq 0 ]]; then
  echo "ERROR: Missing required arguments" >&2
  usage >&2
  exit 1
fi

#------------------------------------------------------------------------------
# Main Workflow
#------------------------------------------------------------------------------

echo "=== Smart Commit-Push Workflow ==="
echo ""

# Step 1: Commit changes
echo "Step 1: Committing changes..."
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/smart-commit.sh ${SMART_COMMIT_ARGS[*]}"
else
  if ! "$SCRIPT_DIR/smart-commit.sh" "${SMART_COMMIT_ARGS[@]}"; then
    echo "ERROR: Commit failed" >&2
    exit 1
  fi
fi

echo ""
echo "Step 2: Fetch and rebase workflow..."
echo "TODO: Implement fetch + rebase + AI conflict resolution"
echo ""

# TODO: Implement the following steps:
# 1. Get list of repos that were committed
# 2. For each repo:
#    a. git fetch origin
#    b. git rebase origin/<branch>
#    c. If conflicts:
#       - Extract conflict markers
#       - Use AI to resolve (similar to AI review)
#       - Apply resolution
#       - git rebase --continue
#    d. git push --force-with-lease

echo "=== Workflow Complete ==="
