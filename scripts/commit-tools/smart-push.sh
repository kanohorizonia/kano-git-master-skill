#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DRY_RUN=0
FORCE_WITH_LEASE=0
NO_VERIFY=0
INCLUDE_ROOT=1
INCLUDE_SUBMODULES=1
INCLUDE_STANDALONE=1
REPO_FILTER=""
NO_SMART_SYNC=0
SYNC_ONLY=0
SKIP_SYNC=0
STASH_LOCAL_CHANGES=0
FAIL_ON_DIRTY_SYNC=0
VERBOSE=0        # Show all repos (default: show only repos with changes)

# Push statistics: format "repo_name|remote|branch"
declare -a PUSH_STATS=()
TIMER_TOTAL_START=0
TIMER_SYNC=0
TIMER_PUSH=0

detect_default_branch() {
  local repo="$1"
  local remote=""
  local head_ref=""
  local branch=""

  for remote in origin upstream; do
    if ! git -C "$repo" remote get-url "$remote" >/dev/null 2>&1; then
      continue
    fi

    head_ref="$(git -C "$repo" symbolic-ref --quiet "refs/remotes/$remote/HEAD" 2>/dev/null || true)"
    if [[ -n "$head_ref" ]]; then
      branch="${head_ref#refs/remotes/$remote/}"
      if [[ -n "$branch" ]]; then
        printf '%s|%s' "$remote" "$branch"
        return 0
      fi
    fi

    for branch in main master dev; do
      if git -C "$repo" show-ref --verify --quiet "refs/remotes/$remote/$branch"; then
        printf '%s|%s' "$remote" "$branch"
        return 0
      fi
    done
  done

  return 1
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
  printf "%-16s  %8s  %s\n" "sync" "$TIMER_SYNC" "$(format_duration "$TIMER_SYNC")"
  printf "%-16s  %8s  %s\n" "push" "$TIMER_PUSH" "$(format_duration "$TIMER_PUSH")"
  printf "%-16s  %8s  %s\n" "total" "$total_elapsed" "$(format_duration "$total_elapsed")"
}

detect_branch_from_superproject_gitmodules() {
  local repo="$1"
  local current=""
  local rel_path=""
  local configured_branch=""

  current="$(dirname "$repo")"
  while [[ -n "$current" ]]; do
    if [[ -f "$current/.gitmodules" ]]; then
      rel_path="${repo#$current/}"
      if [[ "$rel_path" != "$repo" ]]; then
        configured_branch="$(git -C "$current" config -f .gitmodules --get "submodule.$rel_path.branch" 2>/dev/null || true)"
        if [[ -n "$configured_branch" ]]; then
          printf '%s' "$configured_branch"
          return 0
        fi
      fi
    fi

    if [[ "$current" == "/" || "$current" == "." ]]; then
      break
    fi
    current="$(dirname "$current")"
  done

  return 1
}

attach_detached_to_default_branch() {
  local repo="$1"
  local preferred_branch=""
  local detected=""
  local remote=""
  local branch=""

  # 1) Prefer branch explicitly configured in the nearest superproject .gitmodules
  preferred_branch="$(detect_branch_from_superproject_gitmodules "$repo" || true)"
  if [[ -n "$preferred_branch" ]]; then
    if git -C "$repo" show-ref --verify --quiet "refs/heads/$preferred_branch"; then
      git -C "$repo" checkout "$preferred_branch" >/dev/null 2>&1 || true
    elif git -C "$repo" show-ref --verify --quiet "refs/remotes/origin/$preferred_branch"; then
      git -C "$repo" checkout -b "$preferred_branch" "origin/$preferred_branch" >/dev/null 2>&1 || true
    elif git -C "$repo" show-ref --verify --quiet "refs/remotes/upstream/$preferred_branch"; then
      git -C "$repo" checkout -b "$preferred_branch" "upstream/$preferred_branch" >/dev/null 2>&1 || true
    fi

    if git -C "$repo" symbolic-ref --quiet --short HEAD >/dev/null 2>&1; then
      if [[ "$VERBOSE" -eq 1 ]]; then
        echo "[$repo] Attached detached HEAD to .gitmodules branch '$preferred_branch'"
      fi
      return 0
    fi
  fi

  # 2) Fallback to remote default branch detection
  detected="$(detect_default_branch "$repo" || true)"
  if [[ -z "$detected" ]]; then
    return 1
  fi

  remote="${detected%%|*}"
  branch="${detected#*|}"
  if [[ -z "$remote" || -z "$branch" ]]; then
    return 1
  fi

  if git -C "$repo" show-ref --verify --quiet "refs/heads/$branch"; then
    git -C "$repo" checkout "$branch" >/dev/null 2>&1 || return 1
  else
    git -C "$repo" checkout -b "$branch" "$remote/$branch" >/dev/null 2>&1 || return 1
  fi

  if [[ "$VERBOSE" -eq 1 ]]; then
    echo "[$repo] Attached detached HEAD to $branch (from $remote/$branch)"
  fi
  return 0
}

usage() {
  cat <<'EOF'
Usage: smart-push.sh [options]

Push workflow with AI-powered sync and multi-remote push (SSH + HTTP when both exist).

Options:
  --repos <paths>        Only process specific repos (comma-separated)
  --no-root              Exclude root repo
  --no-submodules        Exclude submodules
  --no-standalone        Exclude standalone repos
  --sync-only            Sync only (no push)
  --skip-sync            Push only (skip sync)
  --stash-local-changes  When syncing dirty repos, auto stash before sync and pop after
  --fail-on-dirty-sync   Fail sync if local changes exist (instead of skipping)
  --no-smart-sync        Disable AI-powered sync (use simple git pull --rebase)
  --force-with-lease     Use --force-with-lease on push
  --no-verify            Pass --no-verify to git push (skip pre-push hooks)
  --verbose              Show all repos (default: show only repos with changes)
  --dry-run              Show what would be done without doing it
  -h, --help             Show help

Examples:
  smart-push.sh                    # AI-powered sync + push
  smart-push.sh --no-smart-sync    # Simple rebase + push
  smart-push.sh --repos ".,skills/kano"
  smart-push.sh --no-standalone

Workflow:
  1. Sync with upstream (AI-powered rebase by default)
  2. Push to origin remotes (origin-ssh and origin-http when configured; else origin)

Note:
  By default, uses AI (Copilot/Codex/OpenCode) for intelligent sync.
  Use --no-smart-sync to disable AI and use simple git pull --rebase.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repos)
      REPO_FILTER="${2:-}"
      shift 2
      ;;
    --no-root)
      INCLUDE_ROOT=0
      shift
      ;;
    --no-submodules)
      INCLUDE_SUBMODULES=0
      shift
      ;;
    --no-standalone)
      INCLUDE_STANDALONE=0
      shift
      ;;
    --sync-only)
      SYNC_ONLY=1
      shift
      ;;
    --skip-sync)
      SKIP_SYNC=1
      shift
      ;;
    --stash-local-changes)
      STASH_LOCAL_CHANGES=1
      shift
      ;;
    --fail-on-dirty-sync)
      FAIL_ON_DIRTY_SYNC=1
      shift
      ;;
    --no-smart-sync)
      NO_SMART_SYNC=1
      shift
      ;;
    --force-with-lease)
      FORCE_WITH_LEASE=1
      shift
      ;;
    --no-verify)
      NO_VERIFY=1
      shift
      ;;
    --verbose)
      VERBOSE=1
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
      echo "ERROR: Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ "$SYNC_ONLY" -eq 1 && "$SKIP_SYNC" -eq 1 ]]; then
  echo "ERROR: --sync-only and --skip-sync cannot be used together" >&2
  exit 1
fi
if [[ "$STASH_LOCAL_CHANGES" -eq 1 && "$FAIL_ON_DIRTY_SYNC" -eq 1 ]]; then
  echo "ERROR: --stash-local-changes and --fail-on-dirty-sync cannot be used together" >&2
  exit 1
fi

if [[ -n "${KANO_GIT_MASTER_ROOT:-}" ]]; then
  ROOT="$(cd "$KANO_GIT_MASTER_ROOT" && pwd)"
else
  ROOT="$(cd "$SCRIPT_DIR/../.." && git rev-parse --show-toplevel 2>/dev/null || true)"
  if [[ -z "$ROOT" ]]; then
    ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
  fi
fi

include_types=()
if [[ "$INCLUDE_ROOT" -eq 1 ]]; then
  include_types+=("root")
fi
if [[ "$INCLUDE_SUBMODULES" -eq 1 ]]; then
  include_types+=("submodule")
fi
if [[ "$INCLUDE_STANDALONE" -eq 1 ]]; then
  include_types+=("standalone")
fi

if [[ ${#include_types[@]} -eq 0 ]]; then
  echo "ERROR: All repo types are disabled" >&2
  exit 1
fi

types_csv="$(IFS=,; echo "${include_types[*]}")"

repos_json="$($SCRIPT_DIR/../core/discover-repos.sh --root "$ROOT" --format json --include-types "$types_csv" 2>/dev/null)"

declare -a REPOS=()
MALFORMED_REPO_COUNT=0
while IFS= read -r repo; do
  [[ -z "$repo" ]] && continue
  # Parse path defensively to avoid aborting on malformed entries.
  # Some discovery outputs may include objects without "path".
  path="$(printf '%s' "$repo" | sed -n 's/.*"path":"\([^"]*\)".*/\1/p')"
  if [[ -z "$path" ]]; then
    ((MALFORMED_REPO_COUNT++)) || true
    if [[ "$VERBOSE" -eq 1 ]]; then
      echo "[WARN] Skipping malformed repo entry: $repo" >&2
    fi
    continue
  fi
  REPOS+=("$path")
done < <(echo "$repos_json" | grep -o '{[^}]*}')

if [[ "$VERBOSE" -eq 1 && "$MALFORMED_REPO_COUNT" -gt 0 ]]; then
  echo "[WARN] Skipped $MALFORMED_REPO_COUNT malformed repo entr$( [[ "$MALFORMED_REPO_COUNT" -eq 1 ]] && echo "y" || echo "ies" ) during discovery." >&2
fi

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

if [[ ${#REPOS[@]} -eq 0 ]]; then
  echo "No repositories to push" >&2
  exit 1
fi

FAILED=0
TIMER_TOTAL_START="$(timer_now)"

for repo in "${REPOS[@]}"; do
  branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
  if [[ -z "$branch" ]]; then
    if attach_detached_to_default_branch "$repo"; then
      branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
    fi
  fi

  if [[ -z "$branch" ]]; then
    if [[ "$VERBOSE" -eq 1 ]]; then
      echo "[$repo] SKIP: Detached HEAD"
    fi
    continue
  fi

  if [[ "$VERBOSE" -eq 1 ]]; then
    echo ""
    echo "=== Push: $repo ==="
  fi

  if [[ "$DRY_RUN" -eq 1 ]]; then
    if [[ "$SYNC_ONLY" -eq 1 ]]; then
      echo "[$repo] [DRY RUN] Would sync $branch"
    elif [[ "$SKIP_SYNC" -eq 1 ]]; then
      echo "[$repo] [DRY RUN] Would push $branch (skip sync)"
    else
      echo "[$repo] [DRY RUN] Would sync and push $branch"
    fi
    continue
  fi

  set_upstream=0
  has_upstream=0
  if ! git -C "$repo" rev-parse --abbrev-ref "@{upstream}" >/dev/null 2>&1; then
    set_upstream=1
  else
    has_upstream=1
  fi

  declare -a push_remotes=()
  if git -C "$repo" remote get-url origin-ssh >/dev/null 2>&1; then
    push_remotes+=("origin-ssh")
  fi
  if git -C "$repo" remote get-url origin-http >/dev/null 2>&1; then
    push_remotes+=("origin-http")
  fi
  # Also push plain origin if configured.
  if git -C "$repo" remote get-url origin >/dev/null 2>&1; then
    push_remotes+=("origin")
  fi
  if [[ ${#push_remotes[@]} -eq 0 ]]; then
    echo "[$repo] ERROR: No origin remote found" >&2
    FAILED=1
    continue
  fi

  # Auto-sync with upstream if exists (unless --skip-sync)
  if [[ "$SKIP_SYNC" -eq 0 && "$has_upstream" -eq 1 ]]; then
    sync_step_start="$(timer_now)"
    local_changes=0
    had_stash=0
    sync_output=""
    if [[ -n "$(git -C "$repo" status --porcelain 2>/dev/null || true)" ]]; then
      local_changes=1
    fi

    if [[ "$local_changes" -eq 1 ]]; then
      if [[ "$STASH_LOCAL_CHANGES" -eq 1 ]]; then
        stash_msg="kano-smart-push-autostash $(date +%Y%m%d-%H%M%S)"
        if stash_output="$(git -C "$repo" stash push -u -m "$stash_msg" 2>&1)"; then
          if echo "$stash_output" | grep -q "No local changes to save"; then
            had_stash=0
            if [[ "$VERBOSE" -eq 1 ]]; then
              echo "[$repo] Local changes were not stashable (likely submodule pointer-only state); continuing without stash"
            fi
          else
            had_stash=1
            if [[ "$VERBOSE" -eq 1 ]]; then
              echo "[$repo] Auto-stashed local changes for sync"
            fi
          fi
        else
          echo "[$repo] ERROR: Failed to auto-stash local changes" >&2
          echo "[$repo] Stash output:" >&2
          echo "$stash_output" >&2
          FAILED=1
          continue
        fi
      elif [[ "$FAIL_ON_DIRTY_SYNC" -eq 1 ]]; then
        echo "[$repo] Sync failed: local changes present (dirty working tree)" >&2
        FAILED=1
        continue
      else
        if [[ "$VERBOSE" -eq 1 ]]; then
          echo "[$repo] Sync skipped: local changes present"
        fi
        continue
      fi
    fi

    if sync_output="$(git -C "$repo" pull --rebase 2>&1)"; then
      :
    else
      sync_exit=$?
      echo "[$repo] Sync failed (exit code: $sync_exit)" >&2
      echo "[$repo] Sync output:" >&2
      echo "$sync_output" >&2
      if [[ "$had_stash" -eq 1 ]]; then
        echo "[$repo] Auto-stash kept due to sync failure. Recover with: git -C \"$repo\" stash list" >&2
      fi
      echo "[$repo] Please resolve conflicts manually: cd $repo && git rebase --abort (or continue)" >&2
      FAILED=1
      continue
    fi

    if [[ "$had_stash" -eq 1 ]]; then
      if pop_output="$(git -C "$repo" stash pop 2>&1)"; then
        if [[ "$VERBOSE" -eq 1 ]]; then
          echo "[$repo] Restored stashed changes after sync"
        fi
      else
        pop_exit=$?
        echo "[$repo] ERROR: Auto-stash pop failed (exit code: $pop_exit)" >&2
        echo "[$repo] Stash pop output:" >&2
        echo "$pop_output" >&2
        echo "[$repo] Resolve conflicts and continue manually. Stash may still exist." >&2
        FAILED=1
        continue
      fi
    fi

    # Only print output if there's non-trivial change or in verbose mode
    if [[ -z "${sync_output:-}" ]]; then
      :
    elif echo "$sync_output" | grep -q "Already up to date"; then
      if [[ "$VERBOSE" -eq 1 ]]; then
        echo "[$repo] Already up to date"
      fi
    else
      if [[ "$VERBOSE" -eq 1 ]]; then
        echo "$sync_output" | grep -E "^(Updating|Fast-forward|Already)" || true
        echo "[$repo] Sync successful"
      else
        echo "[$repo] Synced"
      fi
    fi
    TIMER_SYNC=$(( TIMER_SYNC + $(timer_now) - sync_step_start ))
  fi

  if [[ "$SYNC_ONLY" -eq 1 ]]; then
    if [[ "$VERBOSE" -eq 1 ]]; then
      echo "[$repo] Sync-only mode: skipping push"
    fi
    continue
  fi

  push_args=()
  if [[ "$FORCE_WITH_LEASE" -eq 1 ]]; then
    push_args+=("--force-with-lease")
  fi
  if [[ "$NO_VERIFY" -eq 1 ]]; then
    push_args+=("--no-verify")
  fi
  if [[ "$set_upstream" -eq 1 ]]; then
    push_args+=("-u")
  fi

  repo_success=0
  repo_fail_count=0
  add_upstream_flag="$set_upstream"
  last_failed_remote=""
  last_failed_output=""
  short_repo="$(basename "$repo")"
  push_step_start="$(timer_now)"
  for remote_name in "${push_remotes[@]}"; do
    remote_push_args=("${push_args[@]}")
    if [[ "$add_upstream_flag" -eq 0 ]]; then
      filtered=()
      for a in "${remote_push_args[@]}"; do
        [[ "$a" == "-u" ]] && continue
        filtered+=("$a")
      done
      remote_push_args=("${filtered[@]}")
    fi

    if push_output="$(git -C "$repo" push "${remote_push_args[@]}" "$remote_name" "$branch" 2>&1)"; then
      repo_success=1
      add_upstream_flag=0
      if echo "$push_output" | grep -q "Everything up-to-date"; then
        if [[ "$VERBOSE" -eq 1 ]]; then
          echo "[$repo] Push ($remote_name): Everything up-to-date"
        fi
      else
        if [[ "$VERBOSE" -eq 1 ]]; then
          echo "[$repo] Pushed to $remote_name"
        else
          echo "[$repo] Pushed ($remote_name)"
        fi
        PUSH_STATS+=("$short_repo|$remote_name|$branch")
      fi
    else
      repo_fail_count=$((repo_fail_count + 1))
      last_failed_remote="$remote_name"
      last_failed_output="$push_output"
      if [[ "$VERBOSE" -eq 1 ]]; then
        echo "[$repo] Push failed on $remote_name" >&2
        if [[ -n "${push_output:-}" ]]; then
          echo "[$repo] Push output ($remote_name):" >&2
          echo "$push_output" >&2
        fi
      fi
    fi
  done

  if [[ "$repo_success" -eq 0 ]]; then
    echo "[$repo] Push failed on all remotes (${push_remotes[*]})" >&2
    if [[ -n "$last_failed_remote" && -n "$last_failed_output" ]]; then
      echo "[$repo] Last push output ($last_failed_remote):" >&2
      echo "$last_failed_output" >&2
    fi
    FAILED=1
  elif [[ "$repo_fail_count" -gt 0 && "$VERBOSE" -eq 1 ]]; then
    echo "[$repo] Partial push success: $repo_fail_count remote(s) failed, but at least one succeeded"
  fi
  TIMER_PUSH=$(( TIMER_PUSH + $(timer_now) - push_step_start ))
done

if [[ "$FAILED" -eq 1 ]]; then
  # Show push summary before failure
  if [[ "${#PUSH_STATS[@]}" -gt 0 ]]; then
    echo ""
    echo "=== Push Summary (partial) ==="
    printf "%-35s  Remote             Branch\\n" "Repository"
    printf "%-35s  ------             ------\\n" "-----------"
    for stat in "${PUSH_STATS[@]}"; do
      IFS='|' read -r repo_name remote branch <<< "$stat"
      printf "%-35s  %-18s %s\\n" "$repo_name" "$remote" "$branch"
    done
  fi
  print_timing_summary
  exit 1
fi

# Show push summary on success
if [[ "${#PUSH_STATS[@]}" -gt 0 ]]; then
  echo ""
  echo "=== Push Summary ==="
  printf "%-35s  Remote             Branch\\n" "Repository"
  printf "%-35s  ------             ------\\n" "-----------"
  for stat in "${PUSH_STATS[@]}"; do
    IFS='|' read -r repo_name remote branch <<< "$stat"
    printf "%-35s  %-18s %s\\n" "$repo_name" "$remote" "$branch"
  done
fi
print_timing_summary
