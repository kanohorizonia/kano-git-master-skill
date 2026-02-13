#!/usr/bin/env bash
#
# smart-ignore.sh - Orchestrator for smart .gitignore management
#
# Purpose:
#   Coordinate static and AI-driven gitignore management.
#   Executes in order: static-gitignore.sh, then ai-gitignore.sh
#
# Features:
#   - Unified interface for both scripts
#   - Configurable execution (static only, AI only, or both)
#   - Verbose mode for debugging
#   - Dry-run and repo selection support
#
# Usage:
#   ./smart-ignore.sh [options]
#
# Options:
#   --repo <path>              Target repository (default: current repo)
#   --provider <name>          AI provider (opencode, codex, copilot)
#   --model <name>             AI model name
#   --dry-run                  Preview changes without modifying files
#   --no-ai                    Skip AI analysis, static patterns only
#   --no-static                Skip static patterns, AI analysis only
#   --verbose                  Show execution progress
#   -h, --help                 Show this help message
#
# Examples:
#   # Run both static and AI analysis
#   ./smart-ignore.sh --provider copilot --model gpt-4o
#
#   # Static patterns only
#   ./smart-ignore.sh --no-ai
#
#   # AI analysis only
#   ./smart-ignore.sh --no-static --provider copilot --model gpt-4o
#
#   # Dry-run preview
#   ./smart-ignore.sh --provider copilot --model gpt-4o --dry-run --verbose
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Configuration
TARGET_REPO="${PWD}"
AI_PROVIDER=""
AI_MODEL=""
DRY_RUN=0
NO_AI=0
NO_STATIC=0
VERBOSE=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-ignore.sh [options]

Orchestrator for smart .gitignore management with static and AI patterns.

Options:
  --repo <path>              Target repository (default: current repo)
  --provider <name>          AI provider (opencode, codex, copilot)
  --model <name>             AI model name
  --dry-run                  Preview changes without modifying files
  --no-ai                    Skip AI analysis (static only)
  --no-static                Skip static patterns (AI only)
  --verbose                  Show execution progress
  -h, --help                 Show this help message

Examples:
  # Run both static and AI analysis
  ./smart-ignore.sh --provider copilot --model gpt-4o

  # Static patterns only
  ./smart-ignore.sh --no-ai

  # AI analysis only
  ./smart-ignore.sh --no-static --provider copilot --model gpt-4o

  # Dry-run preview with verbose output
  ./smart-ignore.sh --provider copilot --model gpt-4o --dry-run --verbose

Execution Order:
  1. Static patterns (if --no-static not specified)
  2. AI patterns (if --no-ai not specified)

Both managed block types can coexist in the same .gitignore file.
EOF
}

# Parse arguments
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
        echo "ERROR: --provider requires a value" >&2
        exit 1
      fi
      shift 2
      ;;
    --model)
      AI_MODEL="${2:-}"
      if [[ -z "$AI_MODEL" ]]; then
        echo "ERROR: --model requires a value" >&2
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
      echo "ERROR: Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# Validate that we're running at least one script
if [[ "$NO_STATIC" -eq 1 && "$NO_AI" -eq 1 ]]; then
  echo "ERROR: Cannot skip both static and AI analysis" >&2
  exit 1
fi

# If AI is enabled, validate provider and model are specified
if [[ "$NO_AI" -eq 0 && (-z "$AI_PROVIDER" || -z "$AI_MODEL") ]]; then
  echo "ERROR: --provider and --model are required unless --no-ai is specified" >&2
  usage >&2
  exit 1
fi

#------------------------------------------------------------------------------
# Execution
#------------------------------------------------------------------------------

STATIC_SCRIPT="$SCRIPT_DIR/static-gitignore.sh"
AI_SCRIPT="$SCRIPT_DIR/ai-gitignore.sh"

# Verify scripts exist
if [[ ! -f "$STATIC_SCRIPT" ]]; then
  echo "ERROR: static-gitignore.sh not found at $STATIC_SCRIPT" >&2
  exit 1
fi

if [[ ! -f "$AI_SCRIPT" ]]; then
  echo "ERROR: ai-gitignore.sh not found at $AI_SCRIPT" >&2
  exit 1
fi

echo "Smart .gitignore Manager"
echo "======================="
echo "Target: $TARGET_REPO"
[[ "$DRY_RUN" -eq 1 ]] && echo "Mode: DRY-RUN"
echo ""

# Run static patterns
if [[ "$NO_STATIC" -eq 0 ]]; then
  [[ "$VERBOSE" -eq 1 ]] && echo ">>> Running static-gitignore.sh..."
  
  STATIC_ARGS=("--repo" "$TARGET_REPO")
  [[ "$DRY_RUN" -eq 1 ]] && STATIC_ARGS+=("--dry-run")
  
  if ! "$STATIC_SCRIPT" "${STATIC_ARGS[@]}"; then
    echo "WARNING: static-gitignore.sh failed" >&2
  fi
  
  [[ "$VERBOSE" -eq 1 ]] && echo "<<< static-gitignore.sh completed"
  echo ""
fi

# Run AI patterns
if [[ "$NO_AI" -eq 0 ]]; then
  [[ "$VERBOSE" -eq 1 ]] && echo ">>> Running ai-gitignore.sh..."
  
  AI_ARGS=("--provider" "$AI_PROVIDER" "--model" "$AI_MODEL" "--repo" "$TARGET_REPO")
  [[ "$DRY_RUN" -eq 1 ]] && AI_ARGS+=("--dry-run")
  
  if ! "$AI_SCRIPT" "${AI_ARGS[@]}"; then
    echo "WARNING: ai-gitignore.sh failed (provider may not be available)" >&2
  fi
  
  [[ "$VERBOSE" -eq 1 ]] && echo "<<< ai-gitignore.sh completed"
  echo ""
fi

echo "=== All done ==="
exit 0
