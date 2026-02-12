#!/usr/bin/env bash
#
# smart-commit-opencode.sh - OpenCode wrapper for smart-commit
#
# Purpose:
#   Convenience wrapper that calls smart-commit.sh with OpenCode provider
#
# Usage:
#   ./smart-commit-opencode.sh [options]
#
# Options:
#   --model <name>              Override default model (default: auto)
#   --list-models               List available OpenCode models
#   All other options from smart-commit.sh
#
# Examples:
#   # Use default model (auto)
#   ./smart-commit-opencode.sh
#
#   # List available models
#   ./smart-commit-opencode.sh --list-models
#
#   # Use specific model
#   ./smart-commit-opencode.sh --model deepseek-v3.2
#
#   # Commit and push
#   ./smart-commit-opencode.sh --push
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default configuration
DEFAULT_MODEL="auto"

# Parse arguments to extract model override and handle --list-models
MODEL="$DEFAULT_MODEL"
ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --list-models)
      # List models for opencode only
      exec "$SCRIPT_DIR/smart-commit.sh" --list-models opencode
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

# Call smart-commit.sh with OpenCode provider
exec "$SCRIPT_DIR/smart-commit.sh" \
  --provider opencode \
  --model "$MODEL" \
  "${ARGS[@]}"
