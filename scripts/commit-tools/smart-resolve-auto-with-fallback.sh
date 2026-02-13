#!/usr/bin/env bash
#
# smart-resolve-auto-with-fallback.sh - Auto provider selection for conflict resolution
#
# Purpose:
#   Try AI providers in order (Copilot → Codex → OpenCode), fall back to manual if all fail
#
# Provider Order:
#   1. Copilot (gpt-5-mini)
#   2. Codex (gpt-5.3-codex)
#   3. OpenCode (auto)
#   4. Fallback (show conflicts, require manual resolution)
#
# Usage:
#   ./smart-resolve-auto-with-fallback.sh [options]
#
# Options:
#   --interactive               Review each resolution before applying
#   --auto                      Auto-apply all resolutions (default)
#   --file <path>               Only resolve specific file
#   --dry-run                   Show resolutions without applying
#   -h, --help                  Show help
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load AI provider library
source "$SCRIPT_DIR/lib/ai-providers.sh"
source "$SCRIPT_DIR/lib/git-helpers.sh"

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-resolve-auto-with-fallback.sh [options]

Auto-select AI provider for conflict resolution.

Provider Order:
  1. Copilot (gpt-5-mini)
  2. Codex (gpt-5.3-codex)
  3. OpenCode (auto)
  4. Fallback (manual resolution guide)

Options:
  --interactive               Review each resolution before applying
  --auto                      Auto-apply all resolutions (default)
  --file <path>               Only resolve specific file
  --dry-run                   Show resolutions without applying
  -h, --help                  Show help

Examples:
  # Auto-resolve with provider selection
  ./smart-resolve-auto-with-fallback.sh

  # Interactive mode
  ./smart-resolve-auto-with-fallback.sh --interactive

  # Specific file
  ./smart-resolve-auto-with-fallback.sh --file src/main.ts
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

echo "=== Auto Provider Selection (Conflict Resolution) ==="
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

# If AI provider found, use it
if [[ -n "$SELECTED_PROVIDER" ]]; then
  echo "Using AI provider: $SELECTED_PROVIDER (model: $SELECTED_MODEL)"
  echo ""

  exec "$SCRIPT_DIR/smart-resolve.sh" \
    --provider "$SELECTED_PROVIDER" \
    --model "$SELECTED_MODEL" \
    "${ARGS[@]}"
fi

# All AI providers failed - show manual resolution guide
echo "⚠ All AI providers unavailable - manual resolution required"
echo ""

REPO="."

# Validate repository
if ! validate_repo "$REPO"; then
  exit 1
fi

# Check for conflicts
if ! has_conflicts "$REPO"; then
  echo "No conflicts detected"
  exit 0
fi

# Get conflicted files
declare -a files=()
while IFS= read -r file; do
  files+=("$file")
done < <(get_conflicted_files "$REPO")

echo "=== Manual Conflict Resolution Guide ==="
echo ""
echo "Found ${#files[@]} conflicted file(s):"
for file in "${files[@]}"; do
  echo "  - $file"
done
echo ""

echo "Resolution steps:"
echo "  1. Edit each conflicted file"
echo "  2. Look for conflict markers:"
echo "     <<<<<<< HEAD (your changes)"
echo "     ======= (separator)"
echo "     >>>>>>> branch (incoming changes)"
echo "  3. Choose which version to keep or merge manually"
echo "  4. Remove conflict markers"
echo "  5. Stage resolved files: git add <file>"
echo "  6. Continue operation:"

operation="$(get_operation_in_progress "$REPO")"
if [[ "$operation" == "rebase" ]]; then
  echo "     git rebase --continue"
elif [[ "$operation" == "merge" ]]; then
  echo "     git commit"
elif [[ "$operation" == "cherry-pick" ]]; then
  echo "     git cherry-pick --continue"
else
  echo "     git commit"
fi

echo ""
echo "Or install an AI provider for automatic resolution:"
echo "  - Copilot: npm install -g @githubnext/github-copilot-cli"
echo "  - Codex: npm install -g @openai/codex"
echo "  - OpenCode: https://opencode.ai/docs/cli/"

exit 1
