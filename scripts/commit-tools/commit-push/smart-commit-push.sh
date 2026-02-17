#!/usr/bin/env bash
#
#
# Usage:
#   ./smart-commit-push.sh --provider <name> --model <name> [options]
#
# Required Options:
#   --provider <name>           AI provider (opencode, codex, copilot)
#   --model <name>              AI model name
#
# Optional:
#   --agent <name>             Execution identity (manual|codex|copilot|cursor|kiro|claude|...)
#   --repos <paths>             Only process specific repos (comma-separated)
#   --rules <text>              Custom commit rules (inline text)
#   --rules-file <path>         Custom commit rules (from file)
#   -noai                       Short for --no-ai-review
#   --no-ai-review              Disable AI safety review
#   --dry-run                   Show what would be done without doing it
#   -h, --help                  Show help
#
# Examples:
#   # Full workflow with default settings
#   ./smart-commit-push.sh --provider copilot --model gpt-5-mini
#
#   # Only process root and specific submodule
#   ./smart-commit-push.sh --provider copilot --model gpt-5-mini --repos ".,submodules/my-lib"
#
#   # Dry run to see what would happen
#   ./smart-commit-push.sh --provider copilot --model gpt-5-mini --dry-run
#

set -euo pipefail

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Configuration
DRY_RUN=0
SMART_COMMIT_ARGS=()
SMART_PUSH_ARGS=()
AGENT_ID=""
WORKFLOW_ROOT=""
WORKFLOW_LOCK_DIR=""
WORKFLOW_LOCKED=0
TIMER_TOTAL_START=0
TIMER_PRE_SYNC=0
TIMER_COMMIT=0
TIMER_POST_SYNC=0
TIMER_PUSH=0

#------------------------------------------------------------------------------
# Functions
#------------------------------------------------------------------------------

usage() {
  cat <<'EOF'
Usage: smart-commit-push.sh --provider <name> --model <name> [options]

Complete AI-powered workflow: commit → push

Required Options:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Optional:
  --agent <name>             Execution identity. Use "manual" for human-run commands
  --repos <paths>             Only process specific repos (comma-separated)
  --no-root                   Exclude root repo from push
  --no-submodules             Exclude submodules from push
  --no-standalone             Exclude standalone repos from push
  --force-with-lease          Use --force-with-lease when pushing
  --no-verify                 Pass --no-verify to git push (skip pre-push hooks)
  --no-smart-sync             Disable AI-powered sync (use simple git pull --rebase)
  --no-smart-ignore           Disable AI-powered .gitignore updates
  --rules <text>              Custom commit rules (inline text)
  --rules-file <path>         Custom commit rules (from file)
  -noai                       Short for --no-ai-review
  --no-ai-review              Disable AI safety review
  --dry-run                   Show what would be done without doing it
  -h, --help                  Show help

Examples:
  # Full workflow with default settings
  ./smart-commit-push.sh --provider copilot --model gpt-5-mini

  # Only process root and specific submodule
  ./smart-commit-push.sh --provider copilot --model gpt-5-mini --repos ".,submodules/my-lib"

  # Dry run to see what would happen
  ./smart-commit-push.sh --provider copilot --model gpt-5-mini --dry-run

  # Disable AI review with short flag
  ./smart-commit-push.sh --provider copilot --model gpt-5-mini -noai

  # Agent-delegated mode (cost-safe): requires --message, auto-adds --no-ai-review
  ./smart-commit-push.sh --agent codex -m "chore: update workspace"

Workflow Steps:
  1. Pre-sync repositories (stash -> sync -> pop)
  2. Commit changes (using smart-commit.sh)
  3. Post-sync repositories (sync-only, no stash/pop)
  4. Push changes (using smart-push.sh --skip-sync)

Safety:
  - Dry run mode available
EOF
}

truncate_text() {
  local text="$1"
  local max="${2:-80}"
  if (( ${#text} <= max )); then
    printf '%s' "$text"
  else
    printf '%s...' "${text:0:max-3}"
  fi
}

timer_now() {
  date +%s
}

format_duration() {
  local seconds="${1:-0}"
  local h m s
  h=$((seconds / 3600))
  m=$(((seconds % 3600) / 60))
  s=$((seconds % 60))
  if [[ "$h" -gt 0 ]]; then
    printf '%02dh %02dm %02ds' "$h" "$m" "$s"
  elif [[ "$m" -gt 0 ]]; then
    printf '%02dm %02ds' "$m" "$s"
  else
    printf '%02ds' "$s"
  fi
}

print_timing_summary() {
  local total_elapsed
  total_elapsed=$(( $(timer_now) - TIMER_TOTAL_START ))
  echo ""
  echo "=== Timing Summary ==="
  printf "%-16s  %8s  %s\n" "Phase" "Seconds" "Human"
  printf "%-16s  %8s  %s\n" "-----" "-------" "-----"
  printf "%-16s  %8s  %s\n" "pre-sync" "$TIMER_PRE_SYNC" "$(format_duration "$TIMER_PRE_SYNC")"
  printf "%-16s  %8s  %s\n" "commit" "$TIMER_COMMIT" "$(format_duration "$TIMER_COMMIT")"
  printf "%-16s  %8s  %s\n" "post-sync" "$TIMER_POST_SYNC" "$(format_duration "$TIMER_POST_SYNC")"
  printf "%-16s  %8s  %s\n" "push" "$TIMER_PUSH" "$(format_duration "$TIMER_PUSH")"
  printf "%-16s  %8s  %s\n" "total" "$total_elapsed" "$(format_duration "$total_elapsed")"
}

contains_push_arg() {
  local needle="$1"
  local arg=""
  for arg in "${SMART_PUSH_ARGS[@]}"; do
    if [[ "$arg" == "$needle" ]]; then
      return 0
    fi
  done
  return 1
}

get_push_arg_value() {
  local needle="$1"
  local i=0
  for ((i=0; i<${#SMART_PUSH_ARGS[@]}; i++)); do
    if [[ "${SMART_PUSH_ARGS[$i]}" == "$needle" ]]; then
      if [[ $((i+1)) -lt ${#SMART_PUSH_ARGS[@]} ]]; then
        printf '%s' "${SMART_PUSH_ARGS[$((i+1))]}"
        return 0
      fi
      break
    fi
  done
  return 1
}

print_final_workflow_summary() {
  local include_types=()
  local repos_json=""
  local types_csv=""
  local repo_filter=""
  local -a repos=()

  if ! contains_push_arg "--no-root"; then
    include_types+=("root")
  fi
  if ! contains_push_arg "--no-submodules"; then
    include_types+=("submodule")
  fi
  if ! contains_push_arg "--no-standalone"; then
    include_types+=("standalone")
  fi

  if [[ ${#include_types[@]} -eq 0 ]]; then
    return 0
  fi

  types_csv="$(IFS=,; echo "${include_types[*]}")"
  repos_json="$("$SCRIPT_DIR/../../core/discover-repos.sh" --root "$WORKFLOW_ROOT" --format json --include-types "$types_csv" 2>/dev/null || true)"
  while IFS= read -r repo_obj; do
    [[ -z "$repo_obj" ]] && continue
    path="$(printf '%s' "$repo_obj" | sed -n 's/.*"path":"\([^"]*\)".*/\1/p')"
    [[ -z "$path" ]] && continue
    repos+=("$path")
  done < <(echo "$repos_json" | grep -o '{[^}]*}')

  repo_filter="$(get_push_arg_value "--repos" || true)"
  if [[ -n "$repo_filter" ]]; then
    filtered_repos=()
    IFS=',' read -ra filter_paths <<< "$repo_filter"
    for filter_path in "${filter_paths[@]}"; do
      filter_path="${filter_path#./}"
      if [[ "$filter_path" == "." ]]; then
        filter_abs="$WORKFLOW_ROOT"
      elif [[ "$filter_path" == /* ]]; then
        filter_abs="$filter_path"
      else
        filter_abs="$WORKFLOW_ROOT/$filter_path"
      fi
      filter_abs="$(cd "$filter_abs" 2>/dev/null && pwd || echo "$filter_abs")"
      for repo in "${repos[@]}"; do
        if [[ "$repo" == "$filter_abs" ]]; then
          filtered_repos+=("$repo")
          break
        fi
      done
    done
    repos=("${filtered_repos[@]}")
  fi

  if [[ ${#repos[@]} -eq 0 ]]; then
    return 0
  fi

  echo ""
  echo "=== Final Workflow Summary ==="
  printf "%-40s  %-20s  %-8s  %s\n" "Repository" "Branch" "Revision" "HEAD (sha | time | author | title)"
  printf "%-40s  %-20s  %-8s  %s\n" "-----------" "------" "--------" "--------------------------------"

  for repo in "${repos[@]}"; do
    if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      continue
    fi
    repo_rel="$repo"
    if [[ "$repo_rel" == "$WORKFLOW_ROOT" ]]; then
      repo_rel="."
    elif [[ "$repo_rel" == "$WORKFLOW_ROOT/"* ]]; then
      repo_rel="${repo_rel#"$WORKFLOW_ROOT"/}"
    fi
    branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || echo "(detached)")"
    revision="$(git -C "$repo" rev-list --count "$branch" 2>/dev/null || git -C "$repo" rev-list --count HEAD 2>/dev/null || echo "0")"
    head_line="$(git -C "$repo" show -s --format='%h | %cI | %an | %s' HEAD 2>/dev/null || echo "N/A")"
    printf "%-40s  %-20s  %-8s  %s\n" \
      "$(truncate_text "$repo_rel" 40)" \
      "$(truncate_text "$branch" 20)" \
      "$(truncate_text "$revision" 8)" \
      "$(truncate_text "$head_line" 120)"
  done
}

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      SMART_PUSH_ARGS+=("--dry-run")
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --repos|--no-root|--no-submodules|--no-standalone|--force-with-lease|--no-verify|--no-smart-sync)
      SMART_PUSH_ARGS+=("$1")
      if [[ "$1" == "--repos" ]]; then
        SMART_PUSH_ARGS+=("${2:-}")
        shift 2
      else
        shift
      fi
      ;;
    --verbose)
      # Verbose is useful for both commit and push (especially pre-sync diagnostics)
      SMART_COMMIT_ARGS+=("$1")
      SMART_PUSH_ARGS+=("$1")
      shift
      ;;
    --agent)
      AGENT_ID="${2:-}"
      SMART_COMMIT_ARGS+=("$1" "${2:-}")
      shift 2
      ;;
    --no-smart-ignore)
      # Pass to smart-commit.sh
      SMART_COMMIT_ARGS+=("$1")
      shift
      ;;
    -noai)
      SMART_COMMIT_ARGS+=("--no-ai-review")
      shift
      ;;
    *)
      # Pass through to smart-commit.sh
      SMART_COMMIT_ARGS+=("$1")
      shift
      ;;
  esac
done

has_commit_arg() {
  local needle="$1"
  local arg=""
  for arg in "${SMART_COMMIT_ARGS[@]}"; do
    if [[ "$arg" == "$needle" ]]; then
      return 0
    fi
  done
  return 1
}

setup_workflow_root() {
  if [[ -n "${KANO_GIT_MASTER_ROOT:-}" ]]; then
    WORKFLOW_ROOT="$(cd "$KANO_GIT_MASTER_ROOT" && pwd)"
  else
    WORKFLOW_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
    if [[ -z "$WORKFLOW_ROOT" ]]; then
      WORKFLOW_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
    fi
  fi
  WORKFLOW_LOCK_DIR="$WORKFLOW_ROOT/.git/kano-smart-commit-push.lock"
}

release_workflow_lock() {
  if [[ "$WORKFLOW_LOCKED" -eq 1 && -n "$WORKFLOW_LOCK_DIR" ]]; then
    rm -rf "$WORKFLOW_LOCK_DIR"
    WORKFLOW_LOCKED=0
  fi
}

acquire_workflow_lock() {
  if [[ "$DRY_RUN" -eq 1 ]]; then
    return 0
  fi
  if [[ -z "$WORKFLOW_LOCK_DIR" ]]; then
    echo "ERROR: workflow lock path is empty" >&2
    exit 1
  fi
  if mkdir "$WORKFLOW_LOCK_DIR" 2>/dev/null; then
    WORKFLOW_LOCKED=1
    {
      echo "pid=$$"
      echo "agent=${AGENT_ID:-manual}"
      echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$WORKFLOW_LOCK_DIR/owner"
    echo "INFO: workflow lock acquired: $WORKFLOW_LOCK_DIR"
    echo "INFO: workflow in progress. Do not edit files until this command completes."
  else
    echo "ERROR: another smart-commit-push workflow appears active." >&2
    echo "Lock path: $WORKFLOW_LOCK_DIR" >&2
    if [[ -f "$WORKFLOW_LOCK_DIR/owner" ]]; then
      echo "Lock owner info:" >&2
      cat "$WORKFLOW_LOCK_DIR/owner" >&2
    fi
    echo "If stale, remove lock manually after verification." >&2
    exit 1
  fi
}

has_commit_message_arg() {
  local i=0
  local next=""
  for ((i=0; i<${#SMART_COMMIT_ARGS[@]}; i++)); do
    case "${SMART_COMMIT_ARGS[$i]}" in
      -m|--message)
        next=""
        if [[ $((i+1)) -lt ${#SMART_COMMIT_ARGS[@]} ]]; then
          next="${SMART_COMMIT_ARGS[$((i+1))]}"
        fi
        if [[ -n "$next" ]]; then
          return 0
        fi
        ;;
    esac
  done
  return 1
}

# Agent delegation contract pre-check:
# - non-manual --agent requires fixed commit message
# - non-manual --agent auto-injects --no-ai-review if missing
if [[ -n "$AGENT_ID" ]]; then
  AGENT_ID="$(printf '%s' "$AGENT_ID" | tr '[:upper:]' '[:lower:]')"
fi

if [[ -n "$AGENT_ID" && "$AGENT_ID" != "manual" ]]; then
  if ! has_commit_message_arg; then
    echo "ERROR: delegated run requires -m/--message (agent: $AGENT_ID)" >&2
    echo "       Pass a fixed commit message to avoid in-script AI generation." >&2
    exit 1
  fi

  if ! has_commit_arg "--no-ai-review"; then
    SMART_COMMIT_ARGS+=("--no-ai-review")
    echo "INFO: delegated run detected (agent: $AGENT_ID), auto-adding --no-ai-review"
  fi
fi

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
TIMER_TOTAL_START="$(timer_now)"

setup_workflow_root
trap release_workflow_lock EXIT INT TERM
acquire_workflow_lock

# Step 1: Pre-sync changes
echo "Step 1: Pre-sync workflow..."
step_start="$(timer_now)"
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../smart-push.sh --sync-only --stash-local-changes ${SMART_PUSH_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../smart-push.sh" --sync-only --stash-local-changes "${SMART_PUSH_ARGS[@]}"; then
    echo "ERROR: Pre-sync step failed. Check smart-push output above for repository-specific failures." >&2
    echo "Hint: rerun pre-sync with details: ./smart-push.sh --sync-only --verbose" >&2
    echo "After resolving conflicts, rerun this command to continue the 4-step flow." >&2
    exit 1
  fi
fi
TIMER_PRE_SYNC=$(( $(timer_now) - step_start ))

echo ""

# Step 2: Commit changes
echo "Step 2: Committing changes..."
step_start="$(timer_now)"
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../commit/smart-commit.sh ${SMART_COMMIT_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../commit/smart-commit.sh" "${SMART_COMMIT_ARGS[@]}"; then
    echo "ERROR: Commit step failed. Check smart-commit output above for repository-specific failures." >&2
    exit 1
  fi
fi
TIMER_COMMIT=$(( $(timer_now) - step_start ))

echo ""
echo "Step 3: Post-sync workflow..."
step_start="$(timer_now)"
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../smart-push.sh --sync-only --fail-on-dirty-sync ${SMART_PUSH_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../smart-push.sh" --sync-only --fail-on-dirty-sync "${SMART_PUSH_ARGS[@]}"; then
    echo "ERROR: Post-sync step failed. Check smart-push output above for repository-specific failures." >&2
    exit 1
  fi
fi
TIMER_POST_SYNC=$(( $(timer_now) - step_start ))

echo ""
echo "Step 4: Push workflow..."
step_start="$(timer_now)"
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: $SCRIPT_DIR/../smart-push.sh --skip-sync ${SMART_PUSH_ARGS[*]}"
else
  if ! bash "$SCRIPT_DIR/../smart-push.sh" --skip-sync "${SMART_PUSH_ARGS[@]}"; then
    echo "ERROR: Push step failed. Check smart-push output above for repository-specific failures." >&2
    exit 1
  fi
fi
TIMER_PUSH=$(( $(timer_now) - step_start ))

echo ""
echo "=== Workflow Complete (success) ==="
echo "✓ Commit phase completed"
echo "✓ Push phase completed"
print_timing_summary
print_final_workflow_summary
