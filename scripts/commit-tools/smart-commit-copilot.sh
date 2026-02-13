#!/usr/bin/env bash
#
# smart-commit-copilot.sh - Copilot wrapper for smart-commit
#
# Purpose:
#   Convenience wrapper that calls smart-commit.sh with Copilot provider
#
# Usage:
#   ./smart-commit-copilot.sh [options]
#
# Options:
#   --model <name>              Override default model (default: gpt-5-mini)
#   --list-models               List available Copilot models
#   All other options from smart-commit.sh
#
# Examples:
#   # List available models
#   ./smart-commit-copilot.sh --list-models
#
#   # Use default model
#   ./smart-commit-copilot.sh
#
#   # Use specific model
#   ./smart-commit-copilot.sh --model gpt-5-mini
#
#   # Commit and push
#   ./smart-commit-copilot.sh --push
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default configuration
DEFAULT_MODEL="gpt-5-mini"

# Parse arguments to extract model override and handle --list-models
MODEL="$DEFAULT_MODEL"
ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --list-models)
      # List models for copilot only
exec bash "$SCRIPT_DIR/smart-commit.sh" --list-models copilot
      ;;
    --model)
      MODEL="${2:-}"
      shift 2
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

# Call smart-commit.sh with Copilot provider
exec bash "$SCRIPT_DIR/smart-commit.sh" \
  --provider copilot \
  --model "$MODEL" \
  "${ARGS[@]}"
