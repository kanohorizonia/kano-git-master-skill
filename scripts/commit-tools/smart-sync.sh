#!/usr/bin/env bash
#
# smart-sync.sh - AI-powered intelligent sync (rebase-based)
#
# Purpose:
#   Synchronize current branch with upstream using AI-powered rebase
#
# Usage:
#   ./smart-sync.sh --provider <name> --model <name> [options]
#
# Required Options:
#   --provider <name>           AI provider (opencode, codex, copilot)
#   --model <name>              AI model name
#
# Optional:
#   --onto <branch>             Sync onto branch (default: upstream)
#   --interactive               Interactive rebase with AI suggestions
#   --auto-squash               Auto-squash fixup commits
#   --strategy <name>           Rebase strategy (merge, ours, theirs)
#   --dry-run                   Show what would be done
#   -h, --help                  Show help
#
# Examples:
#   # Sync with upstream
#   ./smart-sync.sh --provider copilot --model gpt-5-mini
#
#   # Sync onto specific branch
#   ./smart-sync.sh --provider copilot --model gpt-5-mini --onto main
#
#   # Interactive sync with AI suggestions
#   ./smart-sync.sh --provider copilot --model gpt-5-mini --interactive
#
#   # Auto-squash fixup commits
#   ./smart-sync.sh --provider copilot --model gpt-5-mini --auto-squash
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Load libraries
source "$SCRIPT_DIR/lib/ai-providers.sh"
source "$SCRIPT_DIR/lib/git-helpers.sh"
source "$SCRIPT_DIR/lib/conflict-parser.sh"

# Configuration
AI_PROVIDER=""
AI_MODEL=""
ONTO_BRANCH=""
INTERACTIVE=0
AUTO_SQUASH=0
STRATEGY=""
DRY_RUN=0
REPO="."
NO_SUBMODULE_BRANCH_SYNC=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-sync.sh --provider <name> --model <name> [options]

AI-powered intelligent sync (rebase) operations.

Required Options:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Optional:
  --onto <branch>             Sync onto branch (default: upstream)
  --interactive               Interactive sync with AI suggestions
  --auto-squash               Auto-squash fixup commits
  --strategy <name>           Rebase strategy (merge, ours, theirs)
  --no-submodule-branch-sync  Skip submodule branch sync
  --dry-run                   Show what would be done
  -h, --help                  Show help

Examples:
  # Sync with upstream
  ./smart-sync.sh --provider copilot --model gpt-5-mini

  # Sync onto specific branch
  ./smart-sync.sh --provider copilot --model gpt-5-mini --onto main

  # Interactive sync with AI suggestions
  ./smart-sync.sh --provider copilot --model gpt-5-mini --interactive

  # Auto-squash fixup commits
  ./smart-sync.sh --provider copilot --model gpt-5-mini --auto-squash

Workflow:
  1. Analyze commit history
  2. AI suggests sync strategy
  3. Perform sync operation
  4. Auto-resolve conflicts (if any)
  5. Verify result

AI Features:
  - Suggests which commits to squash
  - Identifies fixup commits
  - Recommends sync strategy
  - Auto-resolves simple conflicts
  - Generates improved commit messages
EOF
}

analyze_commits() {
  local base="$1"
  local head="${2:-HEAD}"

  echo "Analyzing commits (for sync) from $base to $head..."

  # Get commit list
  local commits
  commits="$(git -C "$REPO" log --oneline "$base..$head" 2>/dev/null || true)"

  if [[ -z "$commits" ]]; then
    echo "No commits to sync"
    return 1
  fi

  local commit_count
  commit_count="$(echo "$commits" | wc -l)"

  echo "Found $commit_count commits"
  echo ""
  echo "$commits"
  echo ""

  return 0
}

build_rebase_prompt() {
  local base="$1"
  local head="${2:-HEAD}"

  local commits
  commits="$(git -C "$REPO" log --format="%h %s" "$base..$head" 2>/dev/null || true)"

  cat <<EOF
You are a Git rebase expert.
Analyze these commits and suggest a synchronization (rebase) strategy.

Commits to sync:
$commits

Instructions:
1. Identify commits that should be squashed together
2. Identify fixup/WIP commits
3. Suggest improved commit messages
4. Recommend sync strategy

Output format:
STRATEGY: <merge|squash|rebase>
SQUASH_GROUPS:
- Group 1: <commit1>, <commit2> -> "New message"
- Group 2: <commit3>, <commit4> -> "New message"

FIXUP_COMMITS:
- <commit_hash>: Should be squashed into <target_commit>

RECOMMENDATIONS:
<your recommendations here>
EOF
}

get_ai_rebase_strategy() {
  local base="$1"
  local head="${2:-HEAD}"

  local prompt
  prompt="$(build_rebase_prompt "$base" "$head")"

  echo "Requesting AI sync strategy..."
  local strategy
  strategy="$(ai_generate_message "$AI_PROVIDER" "$AI_MODEL" "$prompt" || true)"

  if [[ -z "$strategy" ]]; then
    echo "WARNING: Failed to get AI strategy, using default" >&2
    return 1
  fi

  echo "$strategy"
  return 0
}

perform_rebase() {
  local target="$1"

  local rebase_args=()

  if [[ "$AUTO_SQUASH" -eq 1 ]]; then
    rebase_args+=(--autosquash)
  fi

  if [[ -n "$STRATEGY" ]]; then
    rebase_args+=(--strategy="$STRATEGY")
  fi

  if [[ "$INTERACTIVE" -eq 1 ]]; then
    rebase_args+=(--interactive)
  fi

  echo "Performing sync (rebase) onto $target..."

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would run: git rebase ${rebase_args[*]} $target"
    return 0
  fi

  # Set editor to non-interactive for auto mode
  if [[ "$INTERACTIVE" -eq 0 ]]; then
    export GIT_EDITOR=:
  fi

  if git -C "$REPO" rebase "${rebase_args[@]}" "$target" 2>/dev/null; then
    echo "Sync successful"
    return 0
  else
    echo "Sync encountered conflicts"

    # Check if conflicts exist
    if has_conflicts "$REPO"; then
      echo "Attempting auto-resolution..."

      # Call smart-resolve if available
      if [[ -f "$SCRIPT_DIR/smart-resolve.sh" ]]; then
        if "$SCRIPT_DIR/smart-resolve.sh" --provider "$AI_PROVIDER" --model "$AI_MODEL" --auto; then
          echo "Conflicts resolved, continuing sync..."
          git -C "$REPO" rebase --continue 2>/dev/null || true
        else
          echo "ERROR: Failed to auto-resolve conflicts" >&2
          echo "Run: git rebase --abort  # to abort" >&2
          echo "Or:  ./smart-resolve.sh --provider $AI_PROVIDER --model $AI_MODEL --interactive" >&2
          return 1
        fi
      else
        echo "ERROR: smart-resolve.sh not found" >&2
        echo "Manual conflict resolution required" >&2
        return 1
      fi
    else
      echo "ERROR: Rebase failed without conflicts" >&2
      return 1
    fi
  fi
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --provider)
      AI_PROVIDER="${2:-}"
      shift 2
      ;;
    --model)
      AI_MODEL="${2:-}"
      shift 2
      ;;
    --onto)
      ONTO_BRANCH="${2:-}"
      shift 2
      ;;
    --interactive)
      INTERACTIVE=1
      shift
      ;;
    --auto-squash)
      AUTO_SQUASH=1
      shift
      ;;
    --strategy)
      STRATEGY="${2:-}"
      shift 2
      ;;
    --no-submodule-branch-sync)
      NO_SUBMODULE_BRANCH_SYNC=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
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

# Validate required parameters
if [[ -z "$AI_PROVIDER" || -z "$AI_MODEL" ]]; then
  echo "ERROR: --provider and --model are required" >&2
  usage >&2
  exit 1
fi

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

echo "=== Smart Sync ==="
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

# Analyze commits
if ! analyze_commits "$target"; then
  exit 0
fi

# Get AI strategy
if [[ "$INTERACTIVE" -eq 0 ]]; then
  ai_strategy="$(get_ai_rebase_strategy "$target" || true)"
  if [[ -n "$ai_strategy" ]]; then
    echo "AI Strategy:"
    echo "$ai_strategy"
    echo ""

    read -p "Apply AI recommendations? [y/N] " -n 1 -r
    echo ""
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
      echo "Proceeding with standard rebase..."
    fi
  fi
fi

# Perform rebase
if ! perform_rebase "$target"; then
  exit 1
fi

if [[ "$NO_SUBMODULE_BRANCH_SYNC" -eq 0 ]]; then
  echo ""
  echo "Syncing submodule branches (if any)..."
  gith_sync_submodules_to_branches "$REPO" "1"
fi

echo ""
echo "=== Sync Complete ==="
echo "Branch $current_branch synced with $target"
