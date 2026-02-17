#!/usr/bin/env bash
#
# smart-sync-dev.sh - Dev maintenance branch migration workflow

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"
source "$SCRIPT_DIR/sync-common.sh"

REPO="."
ORIGIN_REMOTE="origin"
UPSTREAM_REMOTE="upstream"
TARGET_BRANCH=""
SOURCE_BRANCH=""
SOURCE_BRANCH_SET=0
NO_PUSH=0
DRY_RUN=0
CONTINUE_MODE=0
ABORT_MODE=0
NO_AI_RESOLVE=0
RESOLVE_PROVIDER="copilot"
RESOLVE_MODEL="gpt-5-mini"

usage() {
  cat <<'EOF'
Usage: smart-sync-dev.sh [options]

Dev migration workflow:
  - base: upstream default branch tip
  - maintenance branch: branch_<upstream_default_branch>

Options:
  --repo <path>              Target repository path (default: .)
  --origin-remote <name>     Origin remote name (default: origin)
  --upstream-remote <name>   Upstream remote name (default: upstream)
  --target-branch <name>     Target branch (default: branch_<upstream_default>)
  --source-branch <name>     Source branch on origin (default: target branch)
  --no-ai-resolve            Do not auto-resolve cherry-pick conflicts (manual mode)
  --resolve-provider <name>  AI provider for conflict resolve (default: copilot)
  --resolve-model <name>     AI model for conflict resolve (default: gpt-5-mini)
  --no-push                  Do not push target branch
  --continue                 Continue in-progress cherry-pick and push
  --abort                    Abort in-progress cherry-pick
  --dry-run                  Preview actions only
  -h, --help                 Show help
EOF
}

print_dev_sync_summary() {
  local repo="$1"
  local repo_rel="$2"
  local upstream_default="$3"
  local target_branch="$4"
  local planned="$5"
  local applied="$6"
  local skipped="$7"
  local head_commit

  head_commit="$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo "N/A")"
  echo "=== Dev Sync Summary ==="
  echo "repo: $repo_rel"
  echo "upstream_default_branch: $upstream_default"
  echo "branch: $(get_current_branch "$repo")"
  echo "target_branch: $target_branch"
  echo "head_commit: $head_commit"
  echo "planned_commits: $planned"
  echo "applied_commits: $applied"
  echo "skipped_commits: $skipped"
  echo "maintained_commits_local: $(git -C "$repo" rev-list --count "$UPSTREAM_REMOTE/$upstream_default..$target_branch" 2>/dev/null || echo "0")"
}

continue_cherry_pick() {
  local repo="$1"
  local origin_remote="$2"
  local no_ai="$3"
  local provider="$4"
  local model="$5"

  local current_branch
  current_branch="$(get_current_branch "$repo")"
  [[ -z "$current_branch" ]] && echo "ERROR: Detached HEAD during --continue" >&2 && return 1

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would run: git -C $repo cherry-pick --continue"
    [[ "$NO_PUSH" -eq 0 ]] && echo "[DRY RUN] Would run: git -C $repo push -u $origin_remote $current_branch"
    return 0
  fi

  if has_conflicts "$repo"; then
    if [[ "$no_ai" -eq 1 ]]; then
      echo "Conflicts still present. Manual resolve mode enabled (--no-ai-resolve)." >&2
      return 2
    fi
    echo "Conflicts detected; running AI resolver ($provider/$model)..."
    syncc_resolve_conflicts_ai "$SCRIPT_DIR" "$repo" "$provider" "$model"
  fi

  git -C "$repo" cherry-pick --continue
  syncc_push_branch "$repo" "$origin_remote" "$current_branch" "$NO_PUSH"
  echo "Dev sync continue completed on $current_branch"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) REPO="${2:-}"; shift 2 ;;
    --origin-remote) ORIGIN_REMOTE="${2:-}"; shift 2 ;;
    --upstream-remote) UPSTREAM_REMOTE="${2:-}"; shift 2 ;;
    --target-branch) TARGET_BRANCH="${2:-}"; shift 2 ;;
    --source-branch) SOURCE_BRANCH="${2:-}"; SOURCE_BRANCH_SET=1; shift 2 ;;
    --resolve-provider) RESOLVE_PROVIDER="${2:-}"; shift 2 ;;
    --resolve-model) RESOLVE_MODEL="${2:-}"; shift 2 ;;
    --no-ai-resolve) NO_AI_RESOLVE=1; shift ;;
    --no-push) NO_PUSH=1; shift ;;
    --continue) CONTINUE_MODE=1; shift ;;
    --abort) ABORT_MODE=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 1 ;;
  esac
done

validate_repo "$REPO"
WORKSPACE_ROOT="$(syncc_resolve_workspace_root "$REPO")"
REPO_ABS="$(syncc_resolve_repo_abs "$REPO")"
WORKSPACE_ABS="$(cd "$WORKSPACE_ROOT" && pwd -P)"
REPO_REL="$(syncc_resolve_repo_rel_to_root "$REPO_ABS" "$WORKSPACE_ABS")"

if [[ "$CONTINUE_MODE" -eq 1 && "$ABORT_MODE" -eq 1 ]]; then
  echo "ERROR: --continue and --abort cannot be used together" >&2
  exit 1
fi

if [[ "$ABORT_MODE" -eq 1 ]]; then
  if ! is_cherry_pick_in_progress "$REPO"; then echo "ERROR: No cherry-pick in progress in $REPO" >&2; exit 1; fi
  if [[ "$DRY_RUN" -eq 1 ]]; then echo "[DRY RUN] Would run: git -C $REPO cherry-pick --abort"; exit 0; fi
  git -C "$REPO" cherry-pick --abort
  echo "Dev sync cherry-pick aborted"
  exit 0
fi

if [[ "$CONTINUE_MODE" -eq 1 ]]; then
  if ! is_cherry_pick_in_progress "$REPO"; then echo "ERROR: No cherry-pick in progress in $REPO" >&2; exit 1; fi
  continue_cherry_pick "$REPO" "$ORIGIN_REMOTE" "$NO_AI_RESOLVE" "$RESOLVE_PROVIDER" "$RESOLVE_MODEL"
  exit $?
fi

is_clean_working_tree "$REPO" || { echo "ERROR: Working tree has uncommitted changes" >&2; exit 1; }
if is_cherry_pick_in_progress "$REPO" || is_rebase_in_progress "$REPO" || is_merge_in_progress "$REPO"; then
  echo "ERROR: Repository has an in-progress git operation. Resolve it first." >&2
  exit 1
fi

if ! has_remote "$REPO" "$ORIGIN_REMOTE"; then echo "ERROR: Missing origin remote: $ORIGIN_REMOTE" >&2; exit 1; fi
if ! has_remote "$REPO" "$UPSTREAM_REMOTE"; then
  echo "INFO: Repo without upstream detected ($REPO_REL); using fallback branch sync mode."
  syncc_fallback_sync_branch_mode "$REPO" "$ORIGIN_REMOTE" "$WORKSPACE_ABS" "$REPO_ABS" "$DRY_RUN"
  exit $?
fi

git -C "$REPO" fetch "$UPSTREAM_REMOTE" --prune >/dev/null 2>&1 || true
git -C "$REPO" fetch "$ORIGIN_REMOTE" --prune >/dev/null 2>&1 || true

upstream_default="$(syncc_detect_remote_default_branch "$REPO" "$UPSTREAM_REMOTE" || true)"
[[ -z "$upstream_default" ]] && echo "ERROR: Could not detect upstream default branch" >&2 && exit 1

expected_branch="branch_${upstream_default}"
current_branch="$(get_current_branch "$REPO")"
if [[ -z "$current_branch" || "$current_branch" != "$expected_branch" ]]; then
  echo "ERROR: Current branch must be $expected_branch for dev sync migration." >&2
  echo "Current: ${current_branch:-detached}" >&2
  exit 1
fi

if [[ -z "$TARGET_BRANCH" ]]; then TARGET_BRANCH="$expected_branch"; fi
if [[ "$TARGET_BRANCH" != "$expected_branch" ]]; then
  echo "ERROR: target branch must match upstream default mapping: $expected_branch" >&2
  exit 1
fi
if [[ -z "$SOURCE_BRANCH" ]]; then SOURCE_BRANCH="$TARGET_BRANCH"; fi

source_ref=""
if git -C "$REPO" show-ref --verify --quiet "refs/remotes/$ORIGIN_REMOTE/$SOURCE_BRANCH"; then
  source_ref="$ORIGIN_REMOTE/$SOURCE_BRANCH"
elif git -C "$REPO" show-ref --verify --quiet "refs/heads/$SOURCE_BRANCH"; then
  source_ref="$SOURCE_BRANCH"
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "Dev sync plan:"
  echo "  current branch: $current_branch"
  echo "  upstream default branch: $upstream_default"
  echo "  target branch: $TARGET_BRANCH"
  echo "  source branch: ${source_ref:-none (bootstrap mode)}"
fi

declare -a pick_commits=()
if [[ -n "$source_ref" ]]; then
  mapfile -t candidate_commits < <(git -C "$REPO" rev-list --reverse --no-merges "$UPSTREAM_REMOTE/$upstream_default..$source_ref")
  for c in "${candidate_commits[@]}"; do
    pick_commits+=("$c")
  done
fi

echo "Commits to cherry-pick: ${#pick_commits[@]}"
if [[ ${#pick_commits[@]} -gt 0 ]]; then
  for c in "${pick_commits[@]}"; do git -C "$REPO" log -1 --oneline "$c"; done
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would rebuild $TARGET_BRANCH from $UPSTREAM_REMOTE/$upstream_default"
  if [[ ${#pick_commits[@]} -gt 0 ]]; then
    echo "[DRY RUN] Would cherry-pick ${#pick_commits[@]} commits"
  fi
  [[ "$NO_PUSH" -eq 0 ]] && echo "[DRY RUN] Would run: git -C $REPO push -u $ORIGIN_REMOTE $TARGET_BRANCH"
  exit 0
fi

tmp_branch="__sync_dev_tmp_${upstream_default}_$(date +%s)"
git -C "$REPO" checkout -B "$tmp_branch" "$UPSTREAM_REMOTE/$upstream_default" >/dev/null 2>&1

planned_count="${#pick_commits[@]}"
applied_count=0
skipped_count=0
for c in "${pick_commits[@]}"; do
  set +e
  git -C "$REPO" cherry-pick "$c"
  rc=$?
  set -e

  if [[ "$rc" -eq 0 ]]; then
    applied_count=$((applied_count + 1))
    continue
  fi
  if ! has_conflicts "$REPO"; then
    echo "ERROR: cherry-pick failed without conflicts at $c" >&2
    git -C "$REPO" checkout "$current_branch" >/dev/null 2>&1 || true
    git -C "$REPO" branch -D "$tmp_branch" >/dev/null 2>&1 || true
    exit "$rc"
  fi

  echo "Conflict during cherry-pick: $c"
  if [[ "$NO_AI_RESOLVE" -eq 1 ]]; then
    echo "Manual mode (--no-ai-resolve). Resolve conflicts and continue with:" >&2
    echo "  ./smart-sync-dev.sh --repo $REPO --continue --no-ai-resolve" >&2
    exit 2
  fi

  syncc_resolve_conflicts_ai "$SCRIPT_DIR" "$REPO" "$RESOLVE_PROVIDER" "$RESOLVE_MODEL"
  if has_conflicts "$REPO"; then
    echo "ERROR: Conflicts still present after AI resolution." >&2
    exit 2
  fi
  set +e
  git -C "$REPO" cherry-pick --continue
  continue_rc=$?
  set -e
  if [[ "$continue_rc" -eq 0 ]]; then
    applied_count=$((applied_count + 1))
    continue
  fi
  set +e
  git -C "$REPO" cherry-pick --skip
  skip_rc=$?
  set -e
  if [[ "$skip_rc" -eq 0 ]]; then
    skipped_count=$((skipped_count + 1))
    continue
  fi
  echo "ERROR: Failed to continue/skip cherry-pick after conflict resolution." >&2
  exit 2
done

git -C "$REPO" branch -f "$TARGET_BRANCH" "$tmp_branch"
git -C "$REPO" checkout "$TARGET_BRANCH" >/dev/null 2>&1
git -C "$REPO" branch -D "$tmp_branch" >/dev/null 2>&1 || true

syncc_push_branch "$REPO" "$ORIGIN_REMOTE" "$TARGET_BRANCH" "$NO_PUSH"
print_dev_sync_summary "$REPO" "$REPO_REL" "$upstream_default" "$TARGET_BRANCH" "$planned_count" "$applied_count" "$skipped_count"
echo "Dev sync completed: $TARGET_BRANCH"