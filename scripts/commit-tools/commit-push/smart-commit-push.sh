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
#   --agent <name>             Execution identity (manual|codex|copilot|cursor|kiro|claude|...)
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
AGENT_ID=""

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
  --agent <name>             Execution identity. Use "manual" for human-run commands
  --repos <paths>             Only process specific repos (comma-separated)
  --no-root                   Exclude root repo from push
  --no-submodules             Exclude submodules from push
  --no-standalone             Exclude standalone repos from push
  --force-with-lease          Use --force-with-lease when pushing
  --no-smart-sync             Disable AI-powered sync (use simple git pull --rebase)
  --no-smart-ignore           Disable AI-powered .gitignore updates
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

  # Agent-delegated mode (cost-safe): requires --message, auto-adds --no-ai-review
  ./smart-commit-push.sh --agent codex -m "chore: update workspace"

Workflow Steps:
  1. Pre-sync repositories (using smart-push.sh --sync-only)
  2. Commit changes (using smart-commit.sh)
  3. Push changes (using smart-push.sh --skip-sync)

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
    --repos|--no-root|--no-submodules|--no-standalone|--force-with-lease|--no-smart-sync)
      SMART_PUSH_ARGS+=("$1")
      if [[ "$1" == "--repos" ]]; then
        SMART_PUSH_ARGS+=("${2:-}")
        shift 2
      else
        shift
      fi
      ;;
    --agent)
      AGENT_ID="${2:-}"
      SMART_COMMIT_ARGS+=("$1" "${2:-}")
      shift 2
      ;;
    --no-smart-ignore)
      # Pass to smart-commit.sh
      SMART_COMMIT_ARGS+=("$1")
      shift
      ;;
    *)
      # Pass through to smart-commit.sh
      SMART_COMMIT_ARGS+=("$1")
      shift
      ;;
  esac
done

has_commit_arg() {
  local needle="$1"
  local arg=""
  for arg in "${SMART_COMMIT_ARGS[@]}"; do
    if [[ "$arg" == "$needle" ]]; then
      return 0
    fi
  done
  return 1
}

has_commit_message_arg() {
  local i=0
  local next=""
  for ((i=0; i<${#SMART_COMMIT_ARGS[@]}; i++)); do
    case "${SMART_COMMIT_ARGS[$i]}" in
      -m|--message)
        next=""
        if [[ $((i+1)) -lt ${#SMART_COMMIT_ARGS[@]} ]]; then
          next="${SMART_COMMIT_ARGS[$((i+1))]}"
        fi
        if [[ -n "$next" ]]; then
          return 0
        fi
        ;;
    esac
  done
  return 1
}

# Agent delegation contract pre-check:
# - non-manual --agent requires fixed commit message
# - non-manual --agent auto-injects --no-ai-review if missing
if [[ -n "$AGENT_ID" ]]; then
  AGENT_ID="$(printf '%s' "$AGENT_ID" | tr '[:upper:]' '[:lower:]')"
fi

if [[ -n "$AGENT_ID" && "$AGENT_ID" != "manual" ]]; then
  if ! has_commit_message_arg; then
    echo "ERROR: delegated run requires -m/--message (agent: $AGENT_ID)" >&2
    echo "       Pass a fixed commit message to avoid in-script AI generation." >&2
    exit 1
  fi

  if ! has_commit_arg "--no-ai-review"; then
    SMART_COMMIT_ARGS+=("--no-ai-review")
    echo "INFO: delegated run detected (agent: $AGENT_ID), auto-adding --no-ai-review"
  fi
fi

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

# Step 1: Pre-sync changes
echo "Step 1: Pre-sync workflow..."
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../smart-push.sh --sync-only ${SMART_PUSH_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../smart-push.sh" --sync-only "${SMART_PUSH_ARGS[@]}"; then
    echo "ERROR: Pre-sync step failed. Check smart-push output above for repository-specific failures." >&2
    exit 1
  fi
fi

echo ""

# Step 2: Commit changes
echo "Step 2: Committing changes..."
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../commit/smart-commit.sh ${SMART_COMMIT_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../commit/smart-commit.sh" "${SMART_COMMIT_ARGS[@]}"; then
    echo "ERROR: Commit step failed. Check smart-commit output above for repository-specific failures." >&2
    exit 1
  fi
fi

echo ""
echo "Step 3: Push workflow..."
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../smart-push.sh --skip-sync ${SMART_PUSH_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../smart-push.sh" --skip-sync "${SMART_PUSH_ARGS[@]}"; then
    echo "ERROR: Push step failed. Check smart-push output above for repository-specific failures." >&2
    exit 1
  fi
fi

echo ""
echo "=== Workflow Complete (success) ==="
echo "✓ Commit phase completed"
echo "✓ Push phase completed"
