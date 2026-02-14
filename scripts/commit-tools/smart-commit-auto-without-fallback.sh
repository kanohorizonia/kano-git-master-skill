#!/usr/bin/env bash
#
# smart-commit-auto-without-fallback.sh - Auto provider selection (AI only, no fallback)
#
# Purpose:
#   Try AI providers in order (Copilot → Codex → OpenCode), fail if all unavailable
#   REQUIRES at least one AI provider - fails if none available
#
# Provider Order:
#   1. Copilot (gpt-5-mini)
#   2. Codex (gpt-5.3-codex)
#   3. OpenCode (auto)
#
# Usage:
#   ./smart-commit-auto-without-fallback.sh [options]
#
# Options:
#   --ai-review                 Enable AI safety review (default: on)
#   --no-ai-review              Disable AI safety review
#   -f, --push                  Push after commit with --force-with-lease
#   --max-file-size-mb <int>    Block files larger than this (default: 5)
#   --rules <text>              Custom commit rules (inline text)
#   --rules-file <path>         Custom commit rules (from file)
#   --repos <paths>             Only process specific repos (comma-separated paths)
#   -h, --help                  Show help
#
# Examples:
#   # Auto-select provider (AI only)
#   ./smart-commit-auto-without-fallback.sh
#
#   # Commit and push
#   ./smart-commit-auto-without-fallback.sh --push
#
#   # With custom rules
#   ./smart-commit-auto-without-fallback.sh --rules-file .git/commit-rules.md
#
#   # Only specific repos
#   ./smart-commit-auto-without-fallback.sh --repos ".,submodules/my-lib"
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Find repository root
ROOT="$(cd "$SCRIPT_DIR/../.." && git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
fi

# Load AI provider library
LIB_DIR="$SCRIPT_DIR/lib"
if [[ ! -f "$LIB_DIR/ai-providers.sh" ]]; then
  echo "ERROR: AI provider library not found: $LIB_DIR/ai-providers.sh" >&2
  exit 1
fi
source "$LIB_DIR/ai-providers.sh"

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-commit-auto-without-fallback.sh [options]

Auto-select AI provider (AI only, no fallback).

Provider Order:
  1. Copilot (gpt-5-mini)
  2. Codex (gpt-5.3-codex)
  3. OpenCode (auto)

Options:
  --ai-review                 Enable AI safety review (default: on)
  --no-ai-review              Disable AI safety review
  -f, --push                  Push after commit with --force-with-lease
  --max-file-size-mb <int>    Block files larger than this (default: 5)
  --rules <text>              Custom commit rules (inline text)
  --rules-file <path>         Custom commit rules (from file)
  --repos <paths>             Only process specific repos (comma-separated paths)
  -h, --help                  Show help

Examples:
  # Auto-select provider (AI only)
  ./smart-commit-auto-without-fallback.sh

  # Commit and push
  ./smart-commit-auto-without-fallback.sh --push

  # With custom rules
  ./smart-commit-auto-without-fallback.sh --rules-file .git/commit-rules.md

  # Only specific repos
  ./smart-commit-auto-without-fallback.sh --repos ".,submodules/my-lib"

Note:
  This script REQUIRES at least one AI provider. If all providers are
  unavailable, the script will fail with an error.

  For guaranteed success with fallback, use:
    ./smart-commit-auto-with-fallback.sh
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

echo "=== Auto Provider Selection (AI only) ==="
echo ""

# Try providers in order: Copilot → Codex → OpenCode
PROVIDERS=("copilot:gpt-5-mini" "codex:gpt-5.3-codex" "opencode:auto")
SELECTED_PROVIDER=""
SELECTED_MODEL=""

for provider_spec in "${PROVIDERS[@]}"; do
  IFS=':' read -r provider model <<< "$provider_spec"

  echo "Checking $provider..."

  # Check if provider is available
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

This script requires at least one AI provider to be installed:
  - Copilot: npm install -g @githubnext/github-copilot-cli
  - Codex: npm install -g @openai/codex
  - OpenCode: https://opencode.ai/docs/cli/

For guaranteed success with fallback to basic commit, use:
  ./smart-commit-auto-with-fallback.sh
EOF
  exit 1
fi

# Use selected AI provider
echo "Using AI provider: $SELECTED_PROVIDER (model: $SELECTED_MODEL)"
echo ""

exec bash "$SCRIPT_DIR/smart-commit.sh" \
  --provider "$SELECTED_PROVIDER" \
  --model "$SELECTED_MODEL" \
  ${ARGS[@]+"${ARGS[@]}"}
