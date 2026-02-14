#!/usr/bin/env bash
#
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
#   ./smart-commit-push.sh --provider copilot --model gpt-5-mini
#
#   # Only process root and specific submodule
#   ./smart-commit-push.sh --provider copilot --model gpt-5-mini --repos ".,submodules/my-lib"
#
#   # Dry run to see what would happen
#   ./smart-commit-push.sh --provider copilot --model gpt-5-mini --dry-run
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Configuration
DRY_RUN=0
SMART_COMMIT_ARGS=()
SMART_PUSH_ARGS=()

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-commit-push.sh --provider <name> --model <name> [options]

Complete AI-powered workflow: commit → push

Required Options:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Optional:
  --repos <paths>             Only process specific repos (comma-separated)
  --no-root                   Exclude root repo from push
  --no-submodules             Exclude submodules from push
  --no-standalone             Exclude standalone repos from push
  --force-with-lease          Use --force-with-lease when pushing
  --rules <text>              Custom commit rules (inline text)
  --rules-file <path>         Custom commit rules (from file)
  --no-ai-review              Disable AI safety review
  --dry-run                   Show what would be done without doing it
  -h, --help                  Show help

Examples:
  # Full workflow with default settings
  ./smart-commit-push.sh --provider copilot --model gpt-5-mini

  # Only process root and specific submodule
  ./smart-commit-push.sh --provider copilot --model gpt-5-mini --repos ".,submodules/my-lib"

  # Dry run to see what would happen
  ./smart-commit-push.sh --provider copilot --model gpt-5-mini --dry-run

Workflow Steps:
  1. Commit changes (using smart-commit.sh)
  2. Push changes (using smart-push.sh)

Safety:
  - Dry run mode available
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      SMART_PUSH_ARGS+=("--dry-run")
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --repos|--no-root|--no-submodules|--no-standalone|--force-with-lease)
      SMART_PUSH_ARGS+=("$1")
      if [[ "$1" == "--repos" ]]; then
        SMART_PUSH_ARGS+=("${2:-}")
        shift 2
      else
        shift
      fi
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
  if ! bash "$SCRIPT_DIR/smart-commit.sh" "${SMART_COMMIT_ARGS[@]}"; then
    echo "ERROR: Commit step failed. Check smart-commit output above for repository-specific failures." >&2
    exit 1
  fi
fi

echo ""
echo "Step 2: Push workflow..."
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/smart-push.sh ${SMART_PUSH_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/smart-push.sh" "${SMART_PUSH_ARGS[@]}"; then
    echo "ERROR: Push step failed. Check smart-push output above for repository-specific failures." >&2
    exit 1
  fi
fi

echo ""
echo "=== Workflow Complete (success) ==="
