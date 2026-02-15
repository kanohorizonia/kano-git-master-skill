#!/usr/bin/env bash
#
# smart-sync-auto-with-fallback.sh - Auto provider selection for sync
#
# Purpose:
#   Try AI providers in order (Copilot → Codex → OpenCode), fall back to standard sync if all fail
#
# Provider Order:
#   1. Copilot (gpt-5-mini)
#   2. Codex (gpt-5.3-codex)
#   3. OpenCode (auto)
#   4. Fallback (standard git rebase)
#
# Usage:
#   ./smart-sync-auto-with-fallback.sh [options]
#
# Options:
#   --onto <branch>             Sync onto branch (default: upstream)
#   --interactive               Interactive rebase with AI suggestions
#   --auto-squash               Auto-squash fixup commits
#   --strategy <name>           Rebase strategy (merge, ours, theirs)
#   --dry-run                   Show what would be done
#   -h, --help                  Show help
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load libraries
source "$SCRIPT_DIR/lib/ai-providers.sh"
source "$SCRIPT_DIR/lib/git-helpers.sh"

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-sync-auto-with-fallback.sh [options]

Auto-select AI provider for intelligent sync.

Provider Order:
  1. Copilot (gpt-5-mini)
  2. Codex (gpt-5.3-codex)
  3. OpenCode (auto)
  4. Fallback (standard git rebase)

Options:
  --onto <branch>             Rebase onto branch (default: upstream)
  --interactive               Interactive rebase with AI suggestions
  --auto-squash               Auto-squash fixup commits
  --strategy <name>           Rebase strategy (merge, ours, theirs)
  --dry-run                   Show what would be done
  -h, --help                  Show help

Examples:
  # Auto-sync with provider selection
  ./smart-sync-auto-with-fallback.sh

  # Sync onto specific branch
  ./smart-sync-auto-with-fallback.sh --onto main

  # Interactive with AI
  ./smart-sync-auto-with-fallback.sh --interactive
EOF
}

# Parse arguments
ARGS=()
ONTO_BRANCH=""
INTERACTIVE=0
AUTO_SQUASH=0
STRATEGY=""
DRY_RUN=0
REPO="."

while [[ $# -gt 0 ]]; do
  case "$1" in
    --onto)
      ONTO_BRANCH="${2:-}"
      ARGS+=("$1" "$2")
      shift 2
      ;;
    --interactive)
      INTERACTIVE=1
      ARGS+=("$1")
      shift
      ;;
    --auto-squash)
      AUTO_SQUASH=1
      ARGS+=("$1")
      shift
      ;;
    --strategy)
      STRATEGY="${2:-}"
      ARGS+=("$1" "$2")
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      ARGS+=("$1")
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

#------------------------------------------------------------------------------
# Provider Selection
#------------------------------------------------------------------------------

echo "=== Auto Provider Selection (Sync) ==="
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

  exec "$SCRIPT_DIR/smart-sync.sh" \
    --provider "$SELECTED_PROVIDER" \
    --model "$SELECTED_MODEL" \
    "${ARGS[@]}"
fi

# All AI providers failed - use standard git sync (rebase)
echo "⚠ All AI providers unavailable - using standard git sync (rebase)"
echo ""

# Validate repository
if ! validate_repo "$REPO"; then
  exit 1
fi

# Check for clean working tree
if ! is_clean_working_tree "$REPO"; then
  echo "ERROR: Working tree has uncommitted changes" >&2
  echo "Commit or stash changes before syncing" >&2
  exit 1
fi

# Get current branch
current_branch="$(get_current_branch "$REPO")"
if [[ -z "$current_branch" ]]; then
  echo "ERROR: Detached HEAD state" >&2
  exit 1
fi

echo "Current branch: $current_branch"

# Determine target branch
if [[ -n "$ONTO_BRANCH" ]]; then
  target="$ONTO_BRANCH"
else
  target="$(get_upstream_branch "$REPO")"
  if [[ -z "$target" ]]; then
    echo "ERROR: No upstream branch configured" >&2
    echo "Use --onto to specify target branch" >&2
    exit 1
  fi
fi

echo "Target branch: $target"
echo ""

# Build rebase command
rebase_args=()

if [[ "$AUTO_SQUASH" -eq 1 ]]; then
  rebase_args+=(--autosquash)
fi

if [[ -n "$STRATEGY" ]]; then
  rebase_args+=(--strategy="$STRATEGY")
fi

if [[ "$INTERACTIVE" -eq 1 ]]; then
  rebase_args+=(--interactive)
fi

echo "Performing standard sync (rebase)..."

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: git rebase ${rebase_args[*]} $target"
  exit 0
fi

# Set editor to non-interactive for auto mode
if [[ "$INTERACTIVE" -eq 0 ]]; then
  export GIT_EDITOR=:
fi

if git -C "$REPO" rebase "${rebase_args[@]}" "$target"; then
  echo ""
  echo "=== Sync Complete ==="
  echo "Branch $current_branch synced with $target"
else
  echo ""
  echo "ERROR: Rebase failed" >&2
  echo "Resolve conflicts manually or run:" >&2
  echo "  git rebase --abort  # to abort" >&2
  exit 1
fi
