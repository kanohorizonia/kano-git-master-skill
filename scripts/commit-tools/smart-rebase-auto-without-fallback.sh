#!/usr/bin/env bash
#
# smart-rebase-auto-without-fallback.sh - Auto provider selection (AI only)
#
# Purpose:
#   Try AI providers in order (Copilot → Codex → OpenCode), fail if all unavailable
#
# Provider Order:
#   1. Copilot (gpt-5-mini)
#   2. Codex (gpt-5.3-codex)
#   3. OpenCode (auto)
#
# Usage:
#   ./smart-rebase-auto-without-fallback.sh [options]
#
# Options:
#   --onto <branch>             Rebase onto branch (default: upstream)
#   --interactive               Interactive rebase with AI suggestions
#   --auto-squash               Auto-squash fixup commits
#   --strategy <name>           Rebase strategy (merge, ours, theirs)
#   --dry-run                   Show what would be done
#   -h, --help                  Show help
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load AI provider library
source "$SCRIPT_DIR/lib/ai-providers.sh"

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-rebase-auto-without-fallback.sh [options]

Auto-select AI provider for intelligent rebase (AI only, no fallback).

Provider Order:
  1. Copilot (gpt-5-mini)
  2. Codex (gpt-5.3-codex)
  3. OpenCode (auto)

Options:
  --onto <branch>             Rebase onto branch (default: upstream)
  --interactive               Interactive rebase with AI suggestions
  --auto-squash               Auto-squash fixup commits
  --strategy <name>           Rebase strategy (merge, ours, theirs)
  --dry-run                   Show what would be done
  -h, --help                  Show help

Examples:
  # Auto-rebase with provider selection
  ./smart-rebase-auto-without-fallback.sh

  # Rebase onto specific branch
  ./smart-rebase-auto-without-fallback.sh --onto main

  # Interactive with AI
  ./smart-rebase-auto-without-fallback.sh --interactive

Note:
  This script REQUIRES at least one AI provider. If all providers are
  unavailable, the script will fail with an error.

  For standard git rebase fallback, use:
    ./smart-rebase-auto-with-fallback.sh
EOF
}

# Parse arguments
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    *)
      ARGS+=("$1")
      shift
      ;;
  esac
done

#------------------------------------------------------------------------------
# Provider Selection
#------------------------------------------------------------------------------

echo "=== Auto Provider Selection (Rebase - AI only) ==="
echo ""

# Try providers in order
PROVIDERS=("copilot:gpt-5-mini" "codex:gpt-5.3-codex" "opencode:auto")
SELECTED_PROVIDER=""
SELECTED_MODEL=""

for provider_spec in "${PROVIDERS[@]}"; do
  IFS=':' read -r provider model <<< "$provider_spec"

  echo "Checking $provider..."

  case "$provider" in
    copilot)
      if detect_copilot; then
        SELECTED_PROVIDER="$provider"
        SELECTED_MODEL="$model"
        echo "✓ $provider is available (model: $model)"
        break
      else
        echo "✗ $provider not available"
      fi
      ;;
    codex)
      if detect_codex; then
        SELECTED_PROVIDER="$provider"
        SELECTED_MODEL="$model"
        echo "✓ $provider is available (model: $model)"
        break
      else
        echo "✗ $provider not available"
      fi
      ;;
    opencode)
      if detect_opencode; then
        SELECTED_PROVIDER="$provider"
        SELECTED_MODEL="$model"
        echo "✓ $provider is available (model: $model)"
        break
      else
        echo "✗ $provider not available"
      fi
      ;;
  esac
done

echo ""

# If no AI provider found, fail
if [[ -z "$SELECTED_PROVIDER" ]]; then
  cat >&2 <<'EOF'
ERROR: No AI providers available

This script requires at least one AI provider for intelligent rebase:
  - Copilot: npm install -g @githubnext/github-copilot-cli
  - Codex: npm install -g @openai/codex
  - OpenCode: https://opencode.ai/docs/cli/

For standard git rebase fallback, use:
  ./smart-rebase-auto-with-fallback.sh
EOF
  exit 1
fi

# Use selected AI provider
echo "Using AI provider: $SELECTED_PROVIDER (model: $SELECTED_MODEL)"
echo ""

exec "$SCRIPT_DIR/smart-rebase.sh" \
  --provider "$SELECTED_PROVIDER" \
  --model "$SELECTED_MODEL" \
  "${ARGS[@]}"
