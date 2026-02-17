#!/usr/bin/env bash
#
# smart-sync-stable-dev.sh - Stable maintenance branch migration workflow

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"
source "$SCRIPT_DIR/sync-common.sh"

REPO="."
ORIGIN_REMOTE="origin"
UPSTREAM_REMOTE="upstream"
TARGET_TAG=""
BASE_TAG=""
TARGET_BRANCH=""
SOURCE_BRANCH=""
SOURCE_BRANCH_SET=0
RELEASE_CHANNEL="stable"
TAG_PATTERN_STABLE='^(release[-_/])?(v)?[0-9]+(\.[0-9]+){1,3}(\+[0-9A-Za-z.-]+)?$'
TAG_PATTERN_ANY='^(release[-_/])?(v)?[0-9]+(\.[0-9]+){1,3}([.-](alpha|beta|rc|pre|preview)[0-9]*)?(\+[0-9A-Za-z.-]+)?$'
TAG_PATTERN=""
TAG_PATTERN_SET=0
NO_PUSH=0
DRY_RUN=0
CONTINUE_MODE=0
ABORT_MODE=0
NO_AI_RESOLVE=0
RESOLVE_PROVIDER="copilot"
RESOLVE_MODEL="gpt-5-mini"

usage() {
  cat <<'EOF'
Usage: smart-sync-stable-dev.sh [options]

Stable-dev migration workflow:
  - requires clean working tree
  - auto-switches to target stable branch (create if missing)
  - migrates maintenance commits from previous stable branch to latest stable branch

Options:
  --repo <path>              Target repository path (default: .)
  --origin-remote <name>     Origin remote name (default: origin)
  --upstream-remote <name>   Upstream remote name (default: upstream)
  --target-tag <tag>         Target stable tag (default: latest upstream stable tag)
  --base-tag <tag>           Previous stable tag (default: second latest stable tag)
  --target-branch <name>     Target branch (default: branch_<target-tag>)
  --source-branch <name>     Source branch (default: branch_<base-tag>)
  --release-channel <mode>   stable|any (default: stable)
  --tag-pattern <regex>      Regex override for stable tag matching
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

sanitize_tag_for_branch() {
  local tag="$1"
  printf '%s' "$tag" | sed 's#[^A-Za-z0-9._-]#_#g'
}

find_matching_tags_desc() {
  local repo="$1"
  local pattern="$2"
  git -C "$repo" tag --list --sort=-version:refname | grep -Ei "$pattern" || true
}

ensure_local_branch_from_origin() {
  local repo="$1"
  local branch="$2"
  local origin_remote="$3"
  if git -C "$repo" show-ref --verify --quiet "refs/heads/$branch"; then
    return 0
  fi
  if git -C "$repo" show-ref --verify --quiet "refs/remotes/$origin_remote/$branch"; then
    git -C "$repo" checkout -b "$branch" "$origin_remote/$branch" >/dev/null 2>&1
    return 0
  fi
  return 1
}

print_stable_dev_summary() {
  local repo="$1"
  local target_branch="$2"
  local target_tag="$3"
  local base_tag="$4"
  local planned_count="$5"
  local applied_count="$6"
  local skipped_count="$7"
  local conflict_count="$8"
  local resolved_conflict_count="$9"
  local pending_conflict_count="${10}"
  local repo_rel="${11:-$repo}"
  local mode="${12:-stable-dev}"
  local head_commit nearest_tag maintained_count

  head_commit="$(git -C "$repo" rev-parse --short HEAD 2>/dev/null || echo "N/A")"
  nearest_tag="$(git -C "$repo" describe --tags --abbrev=0 2>/dev/null || echo "N/A")"
  maintained_count="$(git -C "$repo" rev-list --count "$target_tag..$target_branch" 2>/dev/null || echo "0")"

  echo "=== Stable Dev Summary ==="
  echo "mode: $mode"
  echo "repo: $repo_rel"
  echo "branch: $(get_current_branch "$repo")"
  echo "target_branch: $target_branch"
  echo "target_version_tag: $target_tag"
  echo "base_version_tag: $base_tag"
  echo "nearest_tag: $nearest_tag"
  echo "head_commit: $head_commit"
  echo "planned_commits: $planned_count"
  echo "applied_commits: $applied_count"
  echo "skipped_commits: $skipped_count"
  echo "conflicts_total: $conflict_count"
  echo "conflicts_resolved: $resolved_conflict_count"
  echo "conflicts_pending: $pending_conflict_count"
  echo "maintained_commits_local: $maintained_count"
}

print_commit_list() {
  local repo="$1"
  local title="$2"
  shift 2
  local commits=("$@")
  [[ ${#commits[@]} -eq 0 ]] && return 0
  echo "$title (${#commits[@]}):"
  for c in "${commits[@]}"; do
    git -C "$repo" log -1 --oneline "$c"
  done
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
      echo "Run: git -C $repo status; resolve files; git -C $repo add ...; git -C $repo cherry-pick --continue" >&2
      return 2
    fi
    echo "Conflicts detected; running AI resolver ($provider/$model)..."
    syncc_resolve_conflicts_ai "$SCRIPT_DIR" "$repo" "$provider" "$model"
  fi

  git -C "$repo" cherry-pick --continue
  syncc_push_branch "$repo" "$origin_remote" "$current_branch" "$NO_PUSH"
  echo "Stable-dev continue completed on $current_branch"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo) REPO="${2:-}"; shift 2 ;;
    --origin-remote) ORIGIN_REMOTE="${2:-}"; shift 2 ;;
    --upstream-remote) UPSTREAM_REMOTE="${2:-}"; shift 2 ;;
    --target-tag) TARGET_TAG="${2:-}"; shift 2 ;;
    --base-tag) BASE_TAG="${2:-}"; shift 2 ;;
    --target-branch) TARGET_BRANCH="${2:-}"; shift 2 ;;
    --source-branch) SOURCE_BRANCH="${2:-}"; SOURCE_BRANCH_SET=1; shift 2 ;;
    --release-channel) RELEASE_CHANNEL="${2:-}"; shift 2 ;;
    --tag-pattern) TAG_PATTERN="${2:-}"; TAG_PATTERN_SET=1; shift 2 ;;
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
  if ! is_cherry_pick_in_progress "$REPO"; then
    echo "ERROR: No cherry-pick in progress in $REPO" >&2
    exit 1
  fi
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would run: git -C $REPO cherry-pick --abort"
    exit 0
  fi
  git -C "$REPO" cherry-pick --abort
  echo "Stable-dev cherry-pick aborted"
  exit 0
fi

if [[ "$CONTINUE_MODE" -eq 1 ]]; then
  if ! is_cherry_pick_in_progress "$REPO"; then
    echo "ERROR: No cherry-pick in progress in $REPO" >&2
    exit 1
  fi
  continue_cherry_pick "$REPO" "$ORIGIN_REMOTE" "$NO_AI_RESOLVE" "$RESOLVE_PROVIDER" "$RESOLVE_MODEL"
  exit $?
fi

is_clean_working_tree "$REPO" || { echo "ERROR: Working tree has uncommitted changes" >&2; exit 1; }
if is_cherry_pick_in_progress "$REPO" || is_rebase_in_progress "$REPO" || is_merge_in_progress "$REPO"; then
  echo "ERROR: Repository has an in-progress git operation. Resolve it first." >&2
  exit 1
fi

if ! has_remote "$REPO" "$ORIGIN_REMOTE"; then
  echo "ERROR: Missing origin remote: $ORIGIN_REMOTE" >&2
  exit 1
fi

if ! has_remote "$REPO" "$UPSTREAM_REMOTE"; then
  echo "INFO: Repo without upstream detected ($REPO_REL); using fallback branch sync mode."
  syncc_fallback_sync_branch_mode "$REPO" "$ORIGIN_REMOTE" "$WORKSPACE_ABS" "$REPO_ABS" "$DRY_RUN"
  exit $?
fi

if [[ "$RELEASE_CHANNEL" != "stable" && "$RELEASE_CHANNEL" != "any" ]]; then
  echo "ERROR: --release-channel must be stable or any" >&2
  exit 1
fi
if [[ "$TAG_PATTERN_SET" -eq 0 ]]; then
  if [[ "$RELEASE_CHANNEL" == "stable" ]]; then TAG_PATTERN="$TAG_PATTERN_STABLE"; else TAG_PATTERN="$TAG_PATTERN_ANY"; fi
fi

git -C "$REPO" fetch "$UPSTREAM_REMOTE" --prune --tags >/dev/null 2>&1 || true
git -C "$REPO" fetch "$ORIGIN_REMOTE" --prune >/dev/null 2>&1 || true

current_branch="$(get_current_branch "$REPO")"

mapfile -t matched_tags < <(find_matching_tags_desc "$REPO" "$TAG_PATTERN")
if [[ ${#matched_tags[@]} -lt 2 && ( -z "$TARGET_TAG" || -z "$BASE_TAG" ) ]]; then
  echo "ERROR: Need at least two stable tags or provide --target-tag and --base-tag." >&2
  exit 1
fi
latest_tag="${matched_tags[0]:-$TARGET_TAG}"
previous_tag="${matched_tags[1]:-$BASE_TAG}"

if [[ -z "$TARGET_TAG" ]]; then TARGET_TAG="$latest_tag"; fi
if [[ -z "$BASE_TAG" ]]; then BASE_TAG="$previous_tag"; fi
if [[ -z "$TARGET_BRANCH" ]]; then TARGET_BRANCH="branch_$(sanitize_tag_for_branch "$TARGET_TAG")"; fi
if [[ -z "$SOURCE_BRANCH" ]]; then SOURCE_BRANCH="branch_$(sanitize_tag_for_branch "$BASE_TAG")"; fi

git -C "$REPO" rev-parse -q --verify "refs/tags/$TARGET_TAG" >/dev/null 2>&1 || { echo "ERROR: Target tag not found: $TARGET_TAG" >&2; exit 1; }
git -C "$REPO" rev-parse -q --verify "refs/tags/$BASE_TAG" >/dev/null 2>&1 || { echo "ERROR: Base tag not found: $BASE_TAG" >&2; exit 1; }

gitmodules_file=""
if [[ "$DRY_RUN" -eq 1 ]]; then
  gitmodules_file="$(syncc_find_gitmodules_file_for_path "$WORKSPACE_ABS" "$REPO_ABS" || true)"
  if [[ -n "$gitmodules_file" ]]; then
    echo "  gitmodules update: would set branch=$TARGET_BRANCH for $REPO_REL"
  fi
else
  gitmodules_file="$(syncc_set_gitmodules_branch_for_path "$WORKSPACE_ABS" "$REPO_ABS" "$TARGET_BRANCH" || true)"
  if [[ -n "$gitmodules_file" ]]; then
    echo "Updated .gitmodules branch for $REPO_REL -> $TARGET_BRANCH ($gitmodules_file)"
  fi
fi

source_ref=""
if git -C "$REPO" show-ref --verify --quiet "refs/remotes/$ORIGIN_REMOTE/$SOURCE_BRANCH"; then
  source_ref="$ORIGIN_REMOTE/$SOURCE_BRANCH"
fi

target_branch_state="new"
if git -C "$REPO" show-ref --verify --quiet "refs/heads/$TARGET_BRANCH"; then
  target_branch_state="existing-local"
elif git -C "$REPO" show-ref --verify --quiet "refs/remotes/$ORIGIN_REMOTE/$TARGET_BRANCH"; then
  target_branch_state="existing-origin"
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "Stable-dev plan:"
  echo "  current branch: $current_branch"
  echo "  latest stable tag: $TARGET_TAG"
  echo "  previous stable tag: $BASE_TAG"
  echo "  target branch: $TARGET_BRANCH"
  echo "  target branch state: $target_branch_state"
  if [[ -n "$source_ref" ]]; then
    echo "  source branch: $source_ref"
  else
    echo "  source branch: (none on origin, bootstrap mode)"
  fi
fi

if [[ "$DRY_RUN" -eq 0 ]]; then
  if [[ "$target_branch_state" == "existing-local" ]]; then
    git -C "$REPO" checkout "$TARGET_BRANCH" >/dev/null 2>&1
  elif [[ "$target_branch_state" == "existing-origin" ]]; then
    ensure_local_branch_from_origin "$REPO" "$TARGET_BRANCH" "$ORIGIN_REMOTE"
  else
    target_branch_state="created-from-tag"
    git -C "$REPO" checkout -b "$TARGET_BRANCH" "tags/$TARGET_TAG" >/dev/null 2>&1
  fi
fi

if [[ "$target_branch_state" == "existing-local" || "$target_branch_state" == "existing-origin" ]]; then
  echo "Target branch already exists ($TARGET_BRANCH); proceeding directly to cherry-pick stage."
fi

declare -a pick_commits=()
if [[ -n "$source_ref" ]]; then
  mapfile -t candidate_commits < <(git -C "$REPO" rev-list --reverse --no-merges "$BASE_TAG..$source_ref")
  for c in "${candidate_commits[@]}"; do
    if git -C "$REPO" merge-base --is-ancestor "$c" "$TARGET_BRANCH" >/dev/null 2>&1; then
      continue
    fi
    pick_commits+=("$c")
  done
else
  echo "INFO: No previous stable branch on origin. Bootstrap mode."
fi

echo "Commits to cherry-pick: ${#pick_commits[@]}"
if [[ ${#pick_commits[@]} -gt 0 ]]; then
  for c in "${pick_commits[@]}"; do git -C "$REPO" log -1 --oneline "$c"; done
fi

if [[ ${#pick_commits[@]} -eq 0 ]]; then
  echo "Already synchronized: no commits to cherry-pick."
  if [[ "$DRY_RUN" -eq 0 ]]; then
    syncc_push_branch "$REPO" "$ORIGIN_REMOTE" "$TARGET_BRANCH" "$NO_PUSH"
    print_stable_dev_summary "$REPO" "$TARGET_BRANCH" "$TARGET_TAG" "$BASE_TAG" "0" "0" "0" "0" "0" "0" "$REPO_REL" "stable-dev"
  fi
  exit 0
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run cherry-pick for ${#pick_commits[@]} commits onto $TARGET_BRANCH"
  [[ "$NO_PUSH" -eq 0 ]] && echo "[DRY RUN] Would run: git -C $REPO push -u $ORIGIN_REMOTE $TARGET_BRANCH"
  exit 0
fi

planned_count="${#pick_commits[@]}"
applied_count=0
skipped_count=0
conflict_count=0
resolved_conflict_count=0
pending_conflict_count=0
declare -a applied_commits=()
declare -a skipped_commits=()
declare -a pending_commits=()

for c in "${pick_commits[@]}"; do
  set +e
  git -C "$REPO" cherry-pick "$c"
  rc=$?
  set -e

  if [[ "$rc" -eq 0 ]]; then
    applied_count=$((applied_count + 1))
    applied_commits+=("$c")
    continue
  fi

  if ! has_conflicts "$REPO"; then
    echo "ERROR: cherry-pick failed without conflicts at $c" >&2
    exit "$rc"
  fi

  echo "Conflict during cherry-pick: $c"
  conflict_count=$((conflict_count + 1))
  if [[ "$NO_AI_RESOLVE" -eq 1 ]]; then
    pending_conflict_count=$((pending_conflict_count + 1))
    pending_commits+=("$c")
    print_stable_dev_summary "$REPO" "$TARGET_BRANCH" "$TARGET_TAG" "$BASE_TAG" "$planned_count" "$applied_count" "$skipped_count" "$conflict_count" "$resolved_conflict_count" "$pending_conflict_count" "$REPO_REL" "stable-dev"
    print_commit_list "$REPO" "Applied commits" "${applied_commits[@]}"
    print_commit_list "$REPO" "Pending conflict commits" "${pending_commits[@]}"
    echo "Manual mode (--no-ai-resolve). Resolve conflicts and continue with:" >&2
    echo "  ./smart-sync-upstream-stable-dev.sh --repo $REPO --continue --no-ai-resolve" >&2
    exit 2
  fi

  syncc_resolve_conflicts_ai "$SCRIPT_DIR" "$REPO" "$RESOLVE_PROVIDER" "$RESOLVE_MODEL"
  if has_conflicts "$REPO"; then
    pending_conflict_count=$((pending_conflict_count + 1))
    pending_commits+=("$c")
    print_stable_dev_summary "$REPO" "$TARGET_BRANCH" "$TARGET_TAG" "$BASE_TAG" "$planned_count" "$applied_count" "$skipped_count" "$conflict_count" "$resolved_conflict_count" "$pending_conflict_count" "$REPO_REL" "stable-dev"
    print_commit_list "$REPO" "Applied commits" "${applied_commits[@]}"
    print_commit_list "$REPO" "Pending conflict commits" "${pending_commits[@]}"
    echo "ERROR: Conflicts still present after AI resolution." >&2
    echo "Run manual continue: ./smart-sync-upstream-stable-dev.sh --repo $REPO --continue --no-ai-resolve" >&2
    exit 2
  fi

  set +e
  git -C "$REPO" cherry-pick --continue
  continue_rc=$?
  set -e
  if [[ "$continue_rc" -eq 0 ]]; then
    resolved_conflict_count=$((resolved_conflict_count + 1))
    applied_count=$((applied_count + 1))
    applied_commits+=("$c")
    continue
  fi

  set +e
  git -C "$REPO" cherry-pick --skip
  skip_rc=$?
  set -e
  if [[ "$skip_rc" -eq 0 ]]; then
    resolved_conflict_count=$((resolved_conflict_count + 1))
    skipped_count=$((skipped_count + 1))
    skipped_commits+=("$c")
    continue
  fi

  pending_conflict_count=$((pending_conflict_count + 1))
  pending_commits+=("$c")
  print_stable_dev_summary "$REPO" "$TARGET_BRANCH" "$TARGET_TAG" "$BASE_TAG" "$planned_count" "$applied_count" "$skipped_count" "$conflict_count" "$resolved_conflict_count" "$pending_conflict_count" "$REPO_REL" "stable-dev"
  print_commit_list "$REPO" "Applied commits" "${applied_commits[@]}"
  print_commit_list "$REPO" "Skipped commits" "${skipped_commits[@]}"
  print_commit_list "$REPO" "Pending conflict commits" "${pending_commits[@]}"
  echo "ERROR: Failed to continue/skip cherry-pick after conflict resolution." >&2
  exit 2
done

syncc_push_branch "$REPO" "$ORIGIN_REMOTE" "$TARGET_BRANCH" "$NO_PUSH"
print_stable_dev_summary "$REPO" "$TARGET_BRANCH" "$TARGET_TAG" "$BASE_TAG" "$planned_count" "$applied_count" "$skipped_count" "$conflict_count" "$resolved_conflict_count" "$pending_conflict_count" "$REPO_REL" "stable-dev"
print_commit_list "$REPO" "Applied commits" "${applied_commits[@]}"
print_commit_list "$REPO" "Skipped commits" "${skipped_commits[@]}"
echo "Stable-dev sync completed: $TARGET_BRANCH"
