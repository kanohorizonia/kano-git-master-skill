#!/usr/bin/env bash
#
# smart-commit-codex.sh - Codex wrapper for smart-commit
#
# Purpose:
#   Convenience wrapper that calls smart-commit.sh with Codex provider
#
# Usage:
#   ./smart-commit-codex.sh [options]
#
# Options:
#   --model <name>              Override default model (default: gpt-5.3-codex)
#   --list-models               List available Codex models
#   All other options from smart-commit.sh
#
# Examples:
#   # Use default model
#   ./smart-commit-codex.sh
#
#   # List available models
#   ./smart-commit-codex.sh --list-models
#
#   # Use specific model
#   ./smart-commit-codex.sh --model gpt-5.2-codex
#
#   # Commit and push
#   ./smart-commit-codex.sh --push
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default configuration
DEFAULT_MODEL="gpt-5.3-codex"

# Parse arguments to extract model override and handle --list-models
MODEL="$DEFAULT_MODEL"
ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --list-models)
      # List models for codex only
      exec "$SCRIPT_DIR/smart-commit.sh" --list-models codex
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

# Call smart-commit.sh with Codex provider
exec "$SCRIPT_DIR/smart-commit.sh" \
  --provider codex \
  --model "$MODEL" \
  "${ARGS[@]}"
