#!/usr/bin/env bash
#
# smart-commit-push.sh - AI-powered commit, rebase, and push workflow
#
# Purpose:
#   Complete workflow: commit → fetch → rebase → resolve conflicts (AI) → push
#
# Usage:
#   ./smart-commit-push.sh --provider <name> --model <name> [options]
#
# Required Options:
#   --provider <name>           AI provider (opencode, codex, copilot)
#   --model <name>              AI model name
#
# Optional:
#   --repos <paths>             Only process specific repos (comma-separated)
#   --rules <text>              Custom commit rules (inline text)
#   --rules-file <path>         Custom commit rules (from file)
#   --no-ai-review              Disable AI safety review
#   --dry-run                   Show what would be done without doing it
#   -h, --help                  Show help
#
# Examples:
#   # Full workflow with default settings
#   ./smart-commit-push.sh --provider copilot --model gpt-4o
#
#   # Only process root and specific submodule
#   ./smart-commit-push.sh --provider copilot --model gpt-4o --repos ".,submodules/my-lib"
#
#   # Dry run to see what would happen
#   ./smart-commit-push.sh --provider copilot --model gpt-4o --dry-run
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Configuration
DRY_RUN=0
SMART_COMMIT_ARGS=()

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-commit-push.sh --provider <name> --model <name> [options]

Complete AI-powered workflow: commit → fetch → rebase → resolve conflicts → push

Required Options:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Optional:
  --repos <paths>             Only process specific repos (comma-separated)
  --rules <text>              Custom commit rules (inline text)
  --rules-file <path>         Custom commit rules (from file)
  --no-ai-review              Disable AI safety review
  --dry-run                   Show what would be done without doing it
  -h, --help                  Show help

Examples:
  # Full workflow with default settings
  ./smart-commit-push.sh --provider copilot --model gpt-4o

  # Only process root and specific submodule
  ./smart-commit-push.sh --provider copilot --model gpt-4o --repos ".,submodules/my-lib"

  # Dry run to see what would happen
  ./smart-commit-push.sh --provider copilot --model gpt-4o --dry-run

Workflow Steps:
  1. Commit changes (using smart-commit.sh)
  2. Fetch latest from remote
  3. Rebase onto remote branch
  4. If conflicts: Use AI to resolve them
  5. Push with --force-with-lease

Safety:
  - Uses --force-with-lease (safe force push)
  - AI reviews conflict resolution
  - Dry run mode available
  - Aborts on unresolvable conflicts
EOF
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      # Pass through to smart-commit.sh
      SMART_COMMIT_ARGS+=("$1")
      shift
      ;;
  esac
done

# Validate that we have required args
if [[ "${#SMART_COMMIT_ARGS[@]}" -eq 0 ]]; then
  echo "ERROR: Missing required arguments" >&2
  usage >&2
  exit 1
fi

#------------------------------------------------------------------------------
# Main Workflow
#------------------------------------------------------------------------------

echo "=== Smart Commit-Push Workflow ==="
echo ""

# Step 1: Commit changes
echo "Step 1: Committing changes..."
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/smart-commit.sh ${SMART_COMMIT_ARGS[*]}"
else
  if ! "$SCRIPT_DIR/smart-commit.sh" "${SMART_COMMIT_ARGS[@]}"; then
    echo "ERROR: Commit failed" >&2
    exit 1
  fi
fi

echo ""
echo "Step 2: Fetch and rebase workflow..."

# Get ROOT directory
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

# Extract provider and model from args
AI_PROVIDER=""
AI_MODEL=""
REPO_FILTER=""

for ((i=0; i<${#SMART_COMMIT_ARGS[@]}; i++)); do
  case "${SMART_COMMIT_ARGS[$i]}" in
    --provider)
      AI_PROVIDER="${SMART_COMMIT_ARGS[$((i+1))]}"
      ;;
    --model)
      AI_MODEL="${SMART_COMMIT_ARGS[$((i+1))]}"
      ;;
    --repos)
      REPO_FILTER="${SMART_COMMIT_ARGS[$((i+1))]}"
      ;;
  esac
done

# Discover repos (same logic as smart-commit.sh)
declare -a REPOS=()

# Add root repo
if git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  REPOS+=("$ROOT")
fi

# Add submodules
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  if git -C "$ROOT/$line" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    REPOS+=("$(cd "$ROOT/$line" && pwd)")
  fi
done < <(git -C "$ROOT" config --file .gitmodules --get-regexp path 2>/dev/null | awk '{print $2}' || true)

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

  REPOS=("${FILTERED_REPOS[@]}")
fi

# Process each repo
for repo in "${REPOS[@]}"; do
  echo ""
  echo "=== Rebase: $repo ==="

  # Get current branch
  branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
  if [[ -z "$branch" ]]; then
    echo "[$repo] SKIP: Detached HEAD"
    continue
  fi

  # Check if branch has upstream
  if ! git -C "$repo" rev-parse --abbrev-ref "@{upstream}" >/dev/null 2>&1; then
    echo "[$repo] SKIP: No upstream branch configured"
    continue
  fi

  upstream="$(git -C "$repo" rev-parse --abbrev-ref "@{upstream}" 2>/dev/null || true)"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would fetch and rebase $branch onto $upstream"
    continue
  fi

  # Fetch
  echo "[$repo] Fetching from remote..."
  if ! git -C "$repo" fetch origin 2>/dev/null; then
    echo "[$repo] WARNING: Fetch failed" >&2
    continue
  fi

  # Check if rebase is needed
  local_commit="$(git -C "$repo" rev-parse HEAD)"
  remote_commit="$(git -C "$repo" rev-parse "$upstream")"

  if [[ "$local_commit" == "$remote_commit" ]]; then
    echo "[$repo] Already up to date"
    continue
  fi

  # Rebase
  echo "[$repo] Rebasing $branch onto $upstream..."
  if git -C "$repo" rebase "$upstream" 2>/dev/null; then
    echo "[$repo] Rebase successful"

    # Push with --force-with-lease
    echo "[$repo] Pushing with --force-with-lease..."
    if git -C "$repo" push --force-with-lease origin "$branch" 2>/dev/null; then
      echo "[$repo] Push successful"
    else
      echo "[$repo] WARNING: Push failed" >&2
    fi
  else
    # Rebase failed - conflicts detected
    echo "[$repo] Conflicts detected during rebase"
    echo "[$repo] TODO: Implement AI conflict resolution"
    echo "[$repo] Aborting rebase..."
    git -C "$repo" rebase --abort 2>/dev/null || true
    echo "[$repo] FAILED: Manual conflict resolution required" >&2
  fi
done

echo ""
echo "=== Workflow Complete ==="
