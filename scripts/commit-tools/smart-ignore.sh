#!/usr/bin/env bash
#
# smart-ignore.sh - Orchestrator for static and AI-driven .gitignore management
#
# Purpose:
#   Coordinates execution of static-gitignore.sh and ai-gitignore.sh in sequence,
#   propagating flags appropriately and handling exit codes.
#
# Features:
#   - Executes static patterns first (deterministic)
#   - Optionally follows with AI patterns (context-aware)
#   - Propagates flags to appropriate child scripts
#   - Combined exit code reporting
#   - Verbose execution progress tracking
#
# Usage:
#   ./smart-ignore.sh [options]
#
# Options:
#   --repo <path>              Target repository (default: current repo)
#   --provider <name>          AI provider (opencode, codex, copilot) - passed to AI script
#   --model <name>             AI model name - passed to AI script
#   --dry-run                  Preview changes without modifying files
#   --no-ai                    Skip AI pattern analysis (static only)
#   --no-static                Skip static patterns (AI only)
#   --verbose                  Show script execution progress to stderr
#   -h, --help                 Show this help message
#
# Examples:
#   # Full run with both static and AI patterns
#   ./smart-ignore.sh --provider copilot --model gpt-4o
#
#   # Static patterns only
#   ./smart-ignore.sh --no-ai
#
#   # AI patterns only
#   ./smart-ignore.sh --provider copilot --model gpt-4o --no-static
#
#   # Dry-run preview for both
#   ./smart-ignore.sh --provider copilot --model gpt-4o --dry-run
#
#   # Verbose output
#   ./smart-ignore.sh --provider copilot --model gpt-4o --verbose
#

set -euo pipefail

#------------------------------------------------------------------------------
# Configuration
#------------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default values
TARGET_REPO=""
AI_PROVIDER=""
AI_MODEL=""
DRY_RUN=0
NO_AI=0
NO_STATIC=0
VERBOSE=0

#------------------------------------------------------------------------------
# Usage function
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
smart-ignore.sh - Orchestrator for static and AI-driven .gitignore management

USAGE:
  ./smart-ignore.sh [options]

OPTIONS:
  --repo <path>              Target repository (default: current repo)
  --provider <name>          AI provider (opencode, codex, copilot)
  --model <name>             AI model name
  --dry-run                  Preview changes without modifying files
  --no-ai                    Skip AI pattern analysis (static only)
  --no-static                Skip static patterns (AI only)
  --verbose                  Show script execution progress to stderr
  -h, --help                 Show this help message

EXAMPLES:
  # Full run with both static and AI patterns
  ./smart-ignore.sh --provider copilot --model gpt-4o

  # Static patterns only
  ./smart-ignore.sh --no-ai

  # AI patterns only
  ./smart-ignore.sh --provider copilot --model gpt-4o --no-static

  # Dry-run preview for both
  ./smart-ignore.sh --provider copilot --model gpt-4o --dry-run

  # Verbose output showing execution progress
  ./smart-ignore.sh --provider copilot --model gpt-4o --verbose

EXECUTION ORDER:
  1. Static patterns (deterministic rules)
  2. AI patterns (context-aware, if enabled)

EXIT CODE:
  0 = Success
  1 = Both scripts skipped (--no-static + --no-ai)
  N = Exit code from first failing script

EOF
}

#------------------------------------------------------------------------------
# Logging helpers
#------------------------------------------------------------------------------

log_verbose() {
  if [[ $VERBOSE -eq 1 ]]; then
    echo "[smart-ignore.sh] $*" >&2
  fi
}

#------------------------------------------------------------------------------
# Parse arguments
#------------------------------------------------------------------------------

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)
      TARGET_REPO="${2:-}"
      if [[ -z "$TARGET_REPO" ]]; then
        echo "ERROR: --repo requires a path argument" >&2
        exit 1
      fi
      shift 2
      ;;
    --provider)
      AI_PROVIDER="${2:-}"
      if [[ -z "$AI_PROVIDER" ]]; then
        echo "ERROR: --provider requires a provider name argument" >&2
        exit 1
      fi
      shift 2
      ;;
    --model)
      AI_MODEL="${2:-}"
      if [[ -z "$AI_MODEL" ]]; then
        echo "ERROR: --model requires a model name argument" >&2
        exit 1
      fi
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --no-ai)
      NO_AI=1
      shift
      ;;
    --no-static)
      NO_STATIC=1
      shift
      ;;
    --verbose)
      VERBOSE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

#------------------------------------------------------------------------------
# Validate configuration
#------------------------------------------------------------------------------

# Check if both scripts are disabled
if [[ $NO_STATIC -eq 1 ]] && [[ $NO_AI -eq 1 ]]; then
  echo "ERROR: Cannot skip both static and AI patterns (--no-static + --no-ai)" >&2
  exit 1
fi

# If AI is not disabled, validate that provider and model are set
if [[ $NO_AI -eq 0 ]]; then
  if [[ -z "$AI_PROVIDER" ]] || [[ -z "$AI_MODEL" ]]; then
    echo "ERROR: AI patterns enabled but --provider and --model are required" >&2
    echo "Use --no-ai to skip AI patterns, or provide --provider and --model" >&2
    exit 1
  fi
fi

#------------------------------------------------------------------------------
# Build argument arrays for child scripts
#------------------------------------------------------------------------------

STATIC_ARGS=()
AI_ARGS=()

# Add --repo to both scripts (if provided)
if [[ -n "$TARGET_REPO" ]]; then
  STATIC_ARGS+=(--repo "$TARGET_REPO")
  AI_ARGS+=(--repo "$TARGET_REPO")
fi

# Add --dry-run to both scripts (if enabled)
if [[ $DRY_RUN -eq 1 ]]; then
  STATIC_ARGS+=(--dry-run)
  AI_ARGS+=(--dry-run)
fi

# Add AI-specific arguments
AI_ARGS+=(--provider "$AI_PROVIDER")
AI_ARGS+=(--model "$AI_MODEL")

#------------------------------------------------------------------------------
# Execute scripts
#------------------------------------------------------------------------------

EXIT_CODE_STATIC=0
EXIT_CODE_AI=0

# Execute static patterns first
if [[ $NO_STATIC -eq 0 ]]; then
  log_verbose "Executing static-gitignore.sh..."
  
  if "$SCRIPT_DIR/static-gitignore.sh" "${STATIC_ARGS[@]:-}" || EXIT_CODE_STATIC=$?; then
    log_verbose "static-gitignore.sh completed successfully (exit code: 0)"
  else
    log_verbose "static-gitignore.sh failed (exit code: $EXIT_CODE_STATIC)"
  fi
else
  log_verbose "Skipping static-gitignore.sh (--no-static)"
fi

# Execute AI patterns second (if enabled)
if [[ $NO_AI -eq 0 ]]; then
  log_verbose "Executing ai-gitignore.sh..."
  
  if "$SCRIPT_DIR/ai-gitignore.sh" "${AI_ARGS[@]:-}" || EXIT_CODE_AI=$?; then
    log_verbose "ai-gitignore.sh completed successfully (exit code: 0)"
  else
    log_verbose "ai-gitignore.sh failed (exit code: $EXIT_CODE_AI)"
  fi
else
  log_verbose "Skipping ai-gitignore.sh (--no-ai)"
fi

#------------------------------------------------------------------------------
# Determine combined exit code
#------------------------------------------------------------------------------

# Exit code logic:
# - If static failed, use its exit code
# - Otherwise if AI failed, use its exit code
# - Otherwise success (0)

if [[ $EXIT_CODE_STATIC -ne 0 ]]; then
  log_verbose "Exiting with static-gitignore.sh exit code: $EXIT_CODE_STATIC"
  exit $EXIT_CODE_STATIC
elif [[ $EXIT_CODE_AI -ne 0 ]]; then
  log_verbose "Exiting with ai-gitignore.sh exit code: $EXIT_CODE_AI"
  exit $EXIT_CODE_AI
else
  log_verbose "Both scripts completed successfully"
  exit 0
fi
