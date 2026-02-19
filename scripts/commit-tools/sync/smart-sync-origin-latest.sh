#!/usr/bin/env bash
#
# smart-sync-origin-latest.sh - Sync local default branch or latest release tag
#
# Purpose:
#   Ensure the local repo is synced to one of:
#     1) remote default branch (main/master/...) with pull --rebase
#     2) latest release-like tag (regex-filtered)
#   This is intentionally non-AI and does not push any changes.
#
# Usage:
#   ./smart-sync-origin-latest.sh [options]
#
# Options:
#   --repo <path>        Target repository path (default: .)
#   --remote <name>     Remote to sync from (default: origin)
#   --target <mode>      Sync target: branch|release (default: branch)
#   --release-channel <mode>  stable|any (default: stable)
#   --tag-pattern <re>   Regex override for release tags
#   --auto-stash         Auto stash/pop when dirty (default)
#   --no-auto-stash      Reject when working tree is dirty
#   --submodule-conflict-strategy <mode>
#                       manual|ff-only|newer-date|ours|theirs (default: ff-only)
#   --dry-run           Show what would be done
#   -h, --help          Show help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

REMOTE="origin"
DRY_RUN=0
REPO="."
TARGET_MODE="branch"
RELEASE_CHANNEL="stable"
TAG_PATTERN_STABLE='^(release[-_/])?(v)?[0-9]+(\.[0-9]+){1,3}(\+[0-9A-Za-z.-]+)?$'
TAG_PATTERN_ANY='^(release[-_/])?(v)?[0-9]+(\.[0-9]+){1,3}([.-](alpha|beta|rc|pre|preview)[0-9]*)?(\+[0-9A-Za-z.-]+)?$'
TAG_PATTERN=""
TAG_PATTERN_SET=0
AUTO_STASH=1
HAD_STASH=0
AUTO_STASH_MARKER=""
SUBMODULE_CONFLICT_STRATEGY="ff-only"
declare -a STASHED_REPOS=()

detect_remote_default_branch() {
  local repo="$1"
  local remote="$2"
  local head_ref=""
  local branch=""

  head_ref="$(git -C "$repo" symbolic-ref --quiet "refs/remotes/$remote/HEAD" 2>/dev/null || true)"
  if [[ -n "$head_ref" ]]; then
    branch="${head_ref#refs/remotes/$remote/}"
    if [[ -n "$branch" ]]; then
      printf '%s' "$branch"
      return 0
    fi
  fi

  for branch in main master dev develop trunk; do
    if git -C "$repo" show-ref --verify --quiet "refs/remotes/$remote/$branch" 2>/dev/null; then
      printf '%s' "$branch"
      return 0
    fi
  done

  return 1
}

usage() {
  cat <<'EOF'
Usage: smart-sync-origin-latest.sh [options]

Sync to remote default branch or latest release-like tag (no push).

Options:
  --repo <path>        Target repository path (default: .)
  --remote <name>     Remote to sync from (default: origin)
  --target <mode>      Sync target: branch|release (default: branch)
  --release-channel <mode>  stable|any (default: stable)
  --tag-pattern <re>   Regex override for release tags
  --auto-stash         Auto stash/pop when dirty (default)
  --no-auto-stash      Reject when working tree is dirty
  --submodule-conflict-strategy <mode>
                      manual|ff-only|newer-date|ours|theirs (default: ff-only)
  --dry-run           Show what would be done
  -h, --help          Show help

Examples:
  ./smart-sync-origin-latest.sh
  ./smart-sync-origin-latest.sh --remote origin
  ./smart-sync-origin-latest.sh --target release
  ./smart-sync-origin-latest.sh --target release --release-channel any
EOF
}

is_rebase_in_progress() {
  local repo="$1"
  [[ -d "$repo/.git/rebase-merge" || -d "$repo/.git/rebase-apply" ]]
}

list_unmerged_submodule_paths() {
  local repo="$1"
  git -C "$repo" ls-files -u 2>/dev/null | awk '$1=="160000" {print $4}' | sort -u || true
}

get_conflict_stage_commit() {
  local repo="$1"
  local path="$2"
  local stage="$3"
  git -C "$repo" ls-files -u -- "$path" 2>/dev/null | awk -v s="$stage" '$3==s {print $2; exit}' || true
}

pick_submodule_commit() {
  local repo="$1"
  local path="$2"
  local ours_sha="$3"
  local theirs_sha="$4"
  local strategy="$5"
  local child="$repo/$path"

  if [[ "$strategy" == "ours" ]]; then
    printf '%s' "$ours_sha"
    return 0
  fi
  if [[ "$strategy" == "theirs" ]]; then
    printf '%s' "$theirs_sha"
    return 0
  fi

  if [[ ! -d "$child/.git" ]] && [[ ! -f "$child/.git" ]]; then
    git -C "$repo" submodule update --init -- "$path" >/dev/null 2>&1 || true
  fi

  if ! git -C "$child" cat-file -e "${ours_sha}^{commit}" >/dev/null 2>&1; then
    git -C "$child" fetch --all --tags --prune >/dev/null 2>&1 || true
  fi
  if ! git -C "$child" cat-file -e "${theirs_sha}^{commit}" >/dev/null 2>&1; then
    git -C "$child" fetch --all --tags --prune >/dev/null 2>&1 || true
  fi

  if git -C "$child" merge-base --is-ancestor "$ours_sha" "$theirs_sha" >/dev/null 2>&1; then
    printf '%s' "$theirs_sha"
    return 0
  fi
  if git -C "$child" merge-base --is-ancestor "$theirs_sha" "$ours_sha" >/dev/null 2>&1; then
    printf '%s' "$ours_sha"
    return 0
  fi

  if [[ "$strategy" == "newer-date" ]]; then
    local ours_ts=""
    local theirs_ts=""
    ours_ts="$(git -C "$child" show -s --format=%ct "$ours_sha" 2>/dev/null || true)"
    theirs_ts="$(git -C "$child" show -s --format=%ct "$theirs_sha" 2>/dev/null || true)"
    if [[ -n "$ours_ts" ]] && [[ -n "$theirs_ts" ]]; then
      if [[ "$theirs_ts" -ge "$ours_ts" ]]; then
        printf '%s' "$theirs_sha"
      else
        printf '%s' "$ours_sha"
      fi
      return 0
    fi
  fi

  return 1
}

resolve_submodule_conflict_once() {
  local repo="$1"
  local path="$2"
  local strategy="$3"
  local ours_sha=""
  local theirs_sha=""
  local chosen_sha=""

  ours_sha="$(get_conflict_stage_commit "$repo" "$path" 2)"
  theirs_sha="$(get_conflict_stage_commit "$repo" "$path" 3)"

  if [[ -z "$ours_sha" ]] && [[ -z "$theirs_sha" ]]; then
    return 0
  fi
  if [[ -z "$ours_sha" ]]; then
    git -C "$repo" rm --cached -f -- "$path" >/dev/null 2>&1 || true
    return 0
  fi
  if [[ -z "$theirs_sha" ]]; then
    git -C "$repo" rm --cached -f -- "$path" >/dev/null 2>&1 || true
    return 0
  fi

  chosen_sha="$(pick_submodule_commit "$repo" "$path" "$ours_sha" "$theirs_sha" "$strategy" || true)"
  if [[ -z "$chosen_sha" ]]; then
    return 1
  fi

  git -C "$repo" update-index --cacheinfo 160000 "$chosen_sha" "$path"
  return 0
}

attempt_auto_resolve_submodule_rebase_conflicts() {
  local repo="$1"
  local strategy="$2"
  local max_loops=30
  local loop=0
  local paths=""
  local p=""

  if [[ "$strategy" == "manual" ]]; then
    return 1
  fi
  if ! is_rebase_in_progress "$repo"; then
    return 1
  fi

  while (( loop < max_loops )); do
    loop=$((loop + 1))
    paths="$(list_unmerged_submodule_paths "$repo")"
    if [[ -z "$paths" ]]; then
      return 0
    fi

    while IFS= read -r p; do
      [[ -n "$p" ]] || continue
      echo "Resolving submodule conflict: $p (strategy=$strategy)"
      if ! resolve_submodule_conflict_once "$repo" "$p" "$strategy"; then
        echo "Could not auto-resolve submodule conflict: $p" >&2
        return 1
      fi
    done <<<"$paths"

    if ! GIT_EDITOR=true git -C "$repo" rebase --continue >/dev/null 2>&1; then
      local remaining=""
      remaining="$(git -C "$repo" diff --name-only --diff-filter=U || true)"
      if [[ -n "$remaining" ]]; then
        echo "Rebase still has unresolved conflicts:" >&2
        echo "$remaining" >&2
        return 1
      fi
      if ! is_rebase_in_progress "$repo"; then
        return 0
      fi
    fi

    if ! is_rebase_in_progress "$repo"; then
      return 0
    fi
  done

  echo "Exceeded max attempts while auto-resolving submodule rebase conflicts" >&2
  return 1
}

find_latest_release_tag() {
  local repo="$1"
  local pattern="$2"

  git -C "$repo" tag --list --sort=-version:refname \
    | grep -Ei "$pattern" \
    | head -n1 || true
}

collect_direct_submodule_paths() {
  local repo="$1"
  local gm="$repo/.gitmodules"
  [[ -f "$gm" ]] || return 0
  git -C "$repo" config -f "$gm" --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk '{print $2}' || true
}

collect_index_gitlink_paths() {
  local repo="$1"
  git -C "$repo" ls-files -s 2>/dev/null | awk '$1=="160000" {print $4}' || true
}

repair_stale_submodule_gitlinks() {
  local repo="$1"
  local had_fix=0
  local gm_paths=""
  local gitlinks=""
  local p=""

  gm_paths="$(collect_direct_submodule_paths "$repo")"
  gitlinks="$(collect_index_gitlink_paths "$repo")"

  while IFS= read -r p; do
    [[ -n "$p" ]] || continue
    if ! grep -Fxq "$p" <<<"$gm_paths"; then
      echo "Repairing stale submodule gitlink: $p"
      git -C "$repo" submodule deinit -- "$p" >/dev/null 2>&1 || true
      git -C "$repo" update-index --force-remove -- "$p"
      had_fix=1
    fi
  done <<<"$gitlinks"

  if [[ "$had_fix" -eq 1 ]]; then
    echo "Stale submodule gitlink cleanup complete"
  fi
}

sync_submodules_after_sync() {
  local repo="$1"
  local dry_run="$2"

  if [[ ! -f "$repo/.gitmodules" ]]; then
    return 0
  fi

  if [[ "$dry_run" -eq 1 ]]; then
    echo "[DRY RUN] Would run: git -C \"$repo\" submodule sync --recursive"
    echo "[DRY RUN] Would run: git -C \"$repo\" submodule update --init --recursive"
    echo "[DRY RUN] Would check and remove stale submodule gitlinks"
    return 0
  fi

  git -C "$repo" submodule sync --recursive >/dev/null 2>&1 || true
  git -C "$repo" submodule update --init --recursive >/dev/null 2>&1 || true
  repair_stale_submodule_gitlinks "$repo"
}

auto_stash_one_repo() {
  local repo="$1"
  local marker="$2"

  if is_clean_working_tree "$repo"; then
    return 0
  fi

  local stash_output=""
  stash_output="$(git -C "$repo" stash push -u -m "$marker" 2>&1 || true)"
  if echo "$stash_output" | grep -q "No local changes to save"; then
    return 0
  fi
  if [[ -z "$stash_output" ]]; then
    echo "ERROR: Failed to auto-stash local changes in: $repo" >&2
    return 1
  fi

  local stash_ref=""
  stash_ref="$(git -C "$repo" stash list -n 1 --format='%gd' || true)"
  STASHED_REPOS+=("$repo")
  echo "Auto-stashed [$repo]: ${stash_ref:-stash@{0}}"
  return 0
}

auto_stash_recursive() {
  local repo="$1"
  local marker="$2"

  local sub_path=""
  while IFS= read -r sub_path; do
    [[ -n "$sub_path" ]] || continue
    local child_repo="$repo/$sub_path"
    if [[ -d "$child_repo" ]] && git -C "$child_repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      auto_stash_recursive "$child_repo" "$marker"
      auto_stash_one_repo "$child_repo" "$marker"
    fi
  done < <(collect_direct_submodule_paths "$repo")
}

auto_pop_one_repo() {
  local repo="$1"
  local marker="$2"

  local top_subject=""
  top_subject="$(git -C "$repo" stash list -n 1 --format='%gs' || true)"
  if [[ "$top_subject" != *"$marker"* ]]; then
    return 0
  fi

  if ! git -C "$repo" stash pop; then
    local stash_ref=""
    stash_ref="$(git -C "$repo" stash list -n 1 --format='%gd' || true)"
    echo "ERROR: Auto-stash pop failed in $repo. Resolve manually and apply ${stash_ref:-stash@{0}}." >&2
    return 1
  fi

  echo "Restored auto-stashed changes: $repo"
  return 0
}

auto_pop_all() {
  local marker="$1"
  local i=0
  for ((i=${#STASHED_REPOS[@]}-1; i>=0; i--)); do
    auto_pop_one_repo "${STASHED_REPOS[$i]}" "$marker"
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)
      REPO="${2:-}"
      shift 2
      ;;
    --remote)
      REMOTE="${2:-}"
      shift 2
      ;;
    --target)
      TARGET_MODE="${2:-}"
      shift 2
      ;;
    --release-channel)
      RELEASE_CHANNEL="${2:-}"
      shift 2
      ;;
    --tag-pattern)
      TAG_PATTERN="${2:-}"
      TAG_PATTERN_SET=1
      shift 2
      ;;
    --auto-stash)
      AUTO_STASH=1
      shift
      ;;
    --no-auto-stash)
      AUTO_STASH=0
      shift
      ;;
    --submodule-conflict-strategy)
      SUBMODULE_CONFLICT_STRATEGY="${2:-}"
      shift 2
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

if ! validate_repo "$REPO"; then
  exit 1
fi

if ! is_clean_working_tree "$REPO"; then
  if [[ "$AUTO_STASH" -eq 0 ]]; then
    echo "ERROR: Working tree has uncommitted changes" >&2
    echo "Commit or stash changes before syncing (or use default auto-stash mode)" >&2
    exit 1
  fi

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Working tree is dirty; would auto-stash (root + submodules) before sync and pop after success"
  else
    AUTO_STASH_MARKER="kano-smart-sync-origin-latest-autostash $(date +%Y%m%d-%H%M%S)-$$"
    auto_stash_recursive "$REPO" "$AUTO_STASH_MARKER"
    auto_stash_one_repo "$REPO" "$AUTO_STASH_MARKER"
    if [[ ${#STASHED_REPOS[@]} -gt 0 ]]; then
      HAD_STASH=1
    fi
  fi
fi

if ! git -C "$REPO" remote get-url "$REMOTE" >/dev/null 2>&1; then
  echo "ERROR: Remote not found: $REMOTE" >&2
  echo "Hint: available remotes:" >&2
  git -C "$REPO" remote -v >&2 || true
  exit 1
fi

if [[ "$TARGET_MODE" != "branch" ]] && [[ "$TARGET_MODE" != "release" ]]; then
  echo "ERROR: --target must be branch or release" >&2
  exit 1
fi

if [[ "$RELEASE_CHANNEL" != "stable" ]] && [[ "$RELEASE_CHANNEL" != "any" ]]; then
  echo "ERROR: --release-channel must be stable or any" >&2
  exit 1
fi

case "$SUBMODULE_CONFLICT_STRATEGY" in
  manual|ff-only|newer-date|ours|theirs) ;;
  *)
    echo "ERROR: --submodule-conflict-strategy must be one of: manual|ff-only|newer-date|ours|theirs" >&2
    exit 1
    ;;
esac

if [[ "$TAG_PATTERN_SET" -eq 0 ]]; then
  if [[ "$RELEASE_CHANNEL" == "stable" ]]; then
    TAG_PATTERN="$TAG_PATTERN_STABLE"
  else
    TAG_PATTERN="$TAG_PATTERN_ANY"
  fi
fi

git -C "$REPO" fetch "$REMOTE" --prune --tags >/dev/null 2>&1 || true

if [[ "$TARGET_MODE" == "branch" ]]; then
  default_branch="$(detect_remote_default_branch "$REPO" "$REMOTE" || true)"
  if [[ -z "${default_branch:-}" ]]; then
    echo "ERROR: Could not detect default branch for remote: $REMOTE" >&2
    exit 1
  fi

  if git -C "$REPO" show-ref --verify --quiet "refs/heads/$default_branch"; then
    checkout_cmd=(git -C "$REPO" checkout "$default_branch")
  else
    checkout_cmd=(git -C "$REPO" checkout -b "$default_branch" "$REMOTE/$default_branch")
  fi

  pull_cmd=(git -C "$REPO" pull --rebase "$REMOTE" "$default_branch")

  echo "Syncing to latest branch: $REMOTE/$default_branch"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would run: ${checkout_cmd[*]}"
    echo "[DRY RUN] Would run: ${pull_cmd[*]}"
    exit 0
  fi

  "${checkout_cmd[@]}" >/dev/null 2>&1
  if ! "${pull_cmd[@]}"; then
    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "[DRY RUN] Rebase failed; would attempt submodule conflict auto-resolve (strategy=$SUBMODULE_CONFLICT_STRATEGY)"
      exit 1
    fi

    if ! attempt_auto_resolve_submodule_rebase_conflicts "$REPO" "$SUBMODULE_CONFLICT_STRATEGY"; then
      if [[ "$HAD_STASH" -eq 1 ]]; then
        echo "Auto-stash kept due to sync failure. Recover with: git -C \"<repo>\" stash list" >&2
      fi
      echo "Sync failed during rebase. Resolve conflicts manually or retry with --submodule-conflict-strategy newer-date|ours|theirs" >&2
      exit 1
    fi
  fi

  sync_submodules_after_sync "$REPO" "$DRY_RUN"

  echo "=== Sync Complete ==="
  echo "On branch: $default_branch"
else
  latest_tag="$(find_latest_release_tag "$REPO" "$TAG_PATTERN")"
  if [[ -z "${latest_tag:-}" ]]; then
    echo "ERROR: No release-like tag found on $REPO using pattern:" >&2
    echo "  $TAG_PATTERN" >&2
    exit 1
  fi

  checkout_cmd=(git -C "$REPO" checkout --detach "tags/$latest_tag")
  echo "Syncing to latest release tag: $latest_tag"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would run: ${checkout_cmd[*]}"
    exit 0
  fi
  "${checkout_cmd[@]}" >/dev/null 2>&1
  sync_submodules_after_sync "$REPO" "$DRY_RUN"
  echo "=== Sync Complete ==="
  echo "On release tag: $latest_tag (detached HEAD)"
fi

if [[ "$HAD_STASH" -eq 1 ]]; then
  echo "Restoring auto-stashed changes..."
  if ! auto_pop_all "$AUTO_STASH_MARKER"; then
    exit 1
  fi
fi
