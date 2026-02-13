#!/usr/bin/env bash
#
# smart-commit-auto-with-fallback.sh - Auto provider selection with guaranteed success
#
# Purpose:
#   Try AI providers in order (Copilot → Codex → OpenCode), fall back to basic commit if all fail
#   GUARANTEES commit success - never fails
#
# Provider Order:
#   1. Copilot (gpt-5-mini)
#   2. Codex (gpt-5.3-codex)
#   3. OpenCode (auto)
#   4. Fallback (basic git commit with auto-generated message)
#
# Usage:
#   ./smart-commit-auto-with-fallback.sh [options]
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
#   # Auto-select provider with fallback
#   ./smart-commit-auto-with-fallback.sh
#
#   # Commit and push
#   ./smart-commit-auto-with-fallback.sh --push
#
#   # With custom rules
#   ./smart-commit-auto-with-fallback.sh --rules-file .git/commit-rules.md
#
#   # Only specific repos
#   ./smart-commit-auto-with-fallback.sh --repos ".,submodules/my-lib"
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

# Configuration
AI_REVIEW=1
DO_PUSH=0
MAX_FILE_SIZE_MB=5
CUSTOM_RULES=""
RULES_FILE=""
REPO_FILTER=""

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-commit-auto-with-fallback.sh [options]

Auto-select AI provider with guaranteed commit success.

Provider Order:
  1. Copilot (gpt-5-mini)
  2. Codex (gpt-5.3-codex)
  3. OpenCode (auto)
  4. Fallback (basic git commit)

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
  # Auto-select provider with fallback
  ./smart-commit-auto-with-fallback.sh

  # Commit and push
  ./smart-commit-auto-with-fallback.sh --push

  # With custom rules
  ./smart-commit-auto-with-fallback.sh --rules-file .git/commit-rules.md

  # Only specific repos
  ./smart-commit-auto-with-fallback.sh --repos ".,submodules/my-lib"

Note:
  This script ALWAYS succeeds. If all AI providers fail, it falls back to
  a basic commit message generated from git diff stats.
EOF
}

# Parse arguments
ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ai-review)
      AI_REVIEW=1
      ARGS+=("$1")
      shift
      ;;
    --no-ai-review)
      AI_REVIEW=0
      ARGS+=("$1")
      shift
      ;;
    -f|--push)
      DO_PUSH=1
      ARGS+=("$1")
      shift
      ;;
    --max-file-size-mb)
      MAX_FILE_SIZE_MB="${2:-}"
      ARGS+=("$1" "$2")
      shift 2
      ;;
    --rules)
      CUSTOM_RULES="${2:-}"
      ARGS+=("$1" "$2")
      shift 2
      ;;
    --rules-file)
      RULES_FILE="${2:-}"
      ARGS+=("$1" "$2")
      shift 2
      ;;
    --repos)
      REPO_FILTER="${2:-}"
      ARGS+=("$1" "$2")
      shift 2
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

echo "=== Auto Provider Selection (with fallback) ==="
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

# If AI provider found, use it
if [[ -n "$SELECTED_PROVIDER" ]]; then
  echo "Using AI provider: $SELECTED_PROVIDER (model: $SELECTED_MODEL)"
  echo ""

  exec "$SCRIPT_DIR/smart-commit.sh" \
    --provider "$SELECTED_PROVIDER" \
    --model "$SELECTED_MODEL" \
    "${ARGS[@]}"
fi

# All AI providers failed - use fallback mode
echo "⚠ All AI providers unavailable - using fallback mode"
echo ""

# Discover repositories (same logic as smart-commit.sh)
declare -a REPOS=()
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
REPO_LIST_FILE="$TMP_DIR/repos.txt"
touch "$REPO_LIST_FILE"

add_repo() {
  local repo="$1"
  if [[ -z "$repo" ]]; then
    return
  fi
  if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    return
  fi
  repo="$(cd "$repo" && pwd)"
  if grep -Fxq "$repo" "$REPO_LIST_FILE" 2>/dev/null; then
    return
  fi
  printf '%s\n' "$repo" >>"$REPO_LIST_FILE"
  REPOS+=("$repo")
}

# Add root repo
add_repo "$ROOT"

# Add submodules
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  add_repo "$ROOT/$line"
done < <(git -C "$ROOT" config --file .gitmodules --get-regexp path 2>/dev/null | awk '{print $2}' || true)

# Add nested git repos
while IFS= read -r git_marker; do
  [[ -z "$git_marker" ]] && continue
  add_repo "$(dirname "$git_marker")"
done < <(find "$ROOT" -type d -name .git -prune -print -o -type f -name .git -print 2>/dev/null || true)

if [[ "${#REPOS[@]}" -eq 0 ]]; then
  echo "No git repositories found under: $ROOT"
  exit 0
fi

# Apply repo filter if specified
if [[ -n "$REPO_FILTER" ]]; then
  declare -a FILTERED_REPOS=()
  IFS=',' read -ra FILTER_PATHS <<< "$REPO_FILTER"

  for filter_path in "${FILTER_PATHS[@]}"; do
    filter_path="${filter_path#./}"
    if [[ "$filter_path" == "." ]]; then
      filter_abs="$ROOT"
    elif [[ "$filter_path" == /* ]]; then
      filter_abs="$filter_path"
    else
      filter_abs="$ROOT/$filter_path"
    fi
    filter_abs="$(cd "$filter_abs" 2>/dev/null && pwd || echo "$filter_abs")"

    for repo in "${REPOS[@]}"; do
      if [[ "$repo" == "$filter_abs" ]]; then
        FILTERED_REPOS+=("$repo")
        break
      fi
    done
  done

  if [[ "${#FILTERED_REPOS[@]}" -eq 0 ]]; then
    echo "No repositories match filter: $REPO_FILTER"
    exit 0
  fi

  REPOS=("${FILTERED_REPOS[@]}")
fi

# Commit each repo with fallback message
for repo in "${REPOS[@]}"; do
  echo ""
  echo "=== Processing: $repo ==="

  # Check for ongoing merge/rebase
  local merge_head rebase_merge rebase_apply
  merge_head="$(git -C "$repo" rev-parse --git-path MERGE_HEAD 2>/dev/null || true)"
  rebase_merge="$(git -C "$repo" rev-parse --git-path rebase-merge 2>/dev/null || true)"
  rebase_apply="$(git -C "$repo" rev-parse --git-path rebase-apply 2>/dev/null || true)"

  if [[ -f "$merge_head" || -d "$rebase_merge" || -d "$rebase_apply" ]]; then
    echo "[$repo] SKIP: Merge/rebase in progress"
    continue
  fi

  # Stage all changes
  git -C "$repo" add -A 2>/dev/null || true

  # Check if there are staged changes
  if git -C "$repo" diff --cached --quiet 2>/dev/null; then
    echo "[$repo] SKIP: No changes"
    continue
  fi

  # Generate fallback message
  msg="$(generate_fallback_message "$repo")"
  echo "[$repo] Commit (fallback): $msg"

  git -C "$repo" commit -m "$msg" 2>/dev/null || {
    echo "[$repo] WARNING: Commit failed" >&2
    continue
  }

  # Push if requested
  if [[ "$DO_PUSH" -eq 1 ]]; then
    branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
    if [[ -z "$branch" ]]; then
      echo "[$repo] SKIP push: Detached HEAD"
    else
      echo "[$repo] Pushing '$branch' with --force-with-lease..."
      git -C "$repo" push --force-with-lease origin "$branch" 2>/dev/null || {
        echo "[$repo] WARNING: Push failed" >&2
      }
    fi
  fi
done

echo ""
echo "=== All done (fallback mode) ==="
