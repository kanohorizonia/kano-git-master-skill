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
#   -noai                       Short for --no-ai-review
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
WORKFLOW_ROOT=""
WORKFLOW_LOCK_DIR=""
WORKFLOW_LOCKED=0

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
  --no-verify                 Pass --no-verify to git push (skip pre-push hooks)
  --no-smart-sync             Disable AI-powered sync (use simple git pull --rebase)
  --no-smart-ignore           Disable AI-powered .gitignore updates
  --rules <text>              Custom commit rules (inline text)
  --rules-file <path>         Custom commit rules (from file)
  -noai                       Short for --no-ai-review
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

  # Disable AI review with short flag
  ./smart-commit-push.sh --provider copilot --model gpt-5-mini -noai

  # Agent-delegated mode (cost-safe): requires --message, auto-adds --no-ai-review
  ./smart-commit-push.sh --agent codex -m "chore: update workspace"

Workflow Steps:
  1. Pre-sync repositories (stash -> sync -> pop)
  2. Commit changes (using smart-commit.sh)
  3. Post-sync repositories (sync-only, no stash/pop)
  4. Push changes (using smart-push.sh --skip-sync)

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
    --repos|--no-root|--no-submodules|--no-standalone|--force-with-lease|--no-verify|--no-smart-sync)
      SMART_PUSH_ARGS+=("$1")
      if [[ "$1" == "--repos" ]]; then
        SMART_PUSH_ARGS+=("${2:-}")
        shift 2
      else
        shift
      fi
      ;;
    --verbose)
      # Verbose is useful for both commit and push (especially pre-sync diagnostics)
      SMART_COMMIT_ARGS+=("$1")
      SMART_PUSH_ARGS+=("$1")
      shift
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
    -noai)
      SMART_COMMIT_ARGS+=("--no-ai-review")
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

setup_workflow_root() {
  if [[ -n "${KANO_GIT_MASTER_ROOT:-}" ]]; then
    WORKFLOW_ROOT="$(cd "$KANO_GIT_MASTER_ROOT" && pwd)"
  else
    WORKFLOW_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    if [[ -z "$WORKFLOW_ROOT" ]]; then
      WORKFLOW_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
    fi
  fi
  WORKFLOW_LOCK_DIR="$WORKFLOW_ROOT/.git/kano-smart-commit-push.lock"
}

release_workflow_lock() {
  if [[ "$WORKFLOW_LOCKED" -eq 1 && -n "$WORKFLOW_LOCK_DIR" ]]; then
    rm -rf "$WORKFLOW_LOCK_DIR"
    WORKFLOW_LOCKED=0
  fi
}

acquire_workflow_lock() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    return 0
  fi
  if [[ -z "$WORKFLOW_LOCK_DIR" ]]; then
    echo "ERROR: workflow lock path is empty" >&2
    exit 1
  fi
  if mkdir "$WORKFLOW_LOCK_DIR" 2>/dev/null; then
    WORKFLOW_LOCKED=1
    {
      echo "pid=$$"
      echo "agent=${AGENT_ID:-manual}"
      echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$WORKFLOW_LOCK_DIR/owner"
    echo "INFO: workflow lock acquired: $WORKFLOW_LOCK_DIR"
    echo "INFO: workflow in progress. Do not edit files until this command completes."
  else
    echo "ERROR: another smart-commit-push workflow appears active." >&2
    echo "Lock path: $WORKFLOW_LOCK_DIR" >&2
    if [[ -f "$WORKFLOW_LOCK_DIR/owner" ]]; then
      echo "Lock owner info:" >&2
      cat "$WORKFLOW_LOCK_DIR/owner" >&2
    fi
    echo "If stale, remove lock manually after verification." >&2
    exit 1
  fi
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

setup_workflow_root
trap release_workflow_lock EXIT INT TERM
acquire_workflow_lock

# Step 1: Pre-sync changes
echo "Step 1: Pre-sync workflow..."
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../smart-push.sh --sync-only --stash-local-changes ${SMART_PUSH_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../smart-push.sh" --sync-only --stash-local-changes "${SMART_PUSH_ARGS[@]}"; then
    echo "ERROR: Pre-sync step failed. Check smart-push output above for repository-specific failures." >&2
    echo "Hint: rerun pre-sync with details: ./smart-push.sh --sync-only --verbose" >&2
    echo "After resolving conflicts, rerun this command to continue the 4-step flow." >&2
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
echo "Step 3: Post-sync workflow..."
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../smart-push.sh --sync-only --fail-on-dirty-sync ${SMART_PUSH_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../smart-push.sh" --sync-only --fail-on-dirty-sync "${SMART_PUSH_ARGS[@]}"; then
    echo "ERROR: Post-sync step failed. Check smart-push output above for repository-specific failures." >&2
    exit 1
  fi
fi

echo ""
echo "Step 4: Push workflow..."
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
