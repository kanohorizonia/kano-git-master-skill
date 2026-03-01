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
source "$SCRIPT_DIR/../../lib/git-helpers.sh"
source "$SCRIPT_DIR/sync-common.sh"

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
DISCOVER_MODE_LAST="unknown"

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

resolve_repo_remote() {
  local repo="$1"
  local preferred_remote="$2"
  local remote_name=""

  if [[ -n "$preferred_remote" ]] && git -C "$repo" remote get-url "$preferred_remote" >/dev/null 2>&1; then
    printf '%s' "$preferred_remote"
    return 0
  fi

  if git -C "$repo" remote get-url origin >/dev/null 2>&1; then
    printf 'origin'
    return 0
  fi

  if git -C "$repo" remote get-url upstream >/dev/null 2>&1; then
    printf 'upstream'
    return 0
  fi

  remote_name="$(git -C "$repo" remote | head -n1 || true)"
  if [[ -n "$remote_name" ]]; then
    printf '%s' "$remote_name"
    return 0
  fi

  return 1
}

discover_nested_repo_paths_fullscan() {
  local root="$1"
  local root_abs=""
  local marker=""
  local repo_path=""

  root_abs="$(cd "$root" && pwd -P)"

  while IFS= read -r marker; do
    [[ -n "$marker" ]] || continue
    repo_path="$(dirname "$marker")"
    repo_path="$(cd "$repo_path" && pwd -P)"
    [[ "$repo_path" == "$root_abs" ]] && continue
    printf '%s\n' "$repo_path"
  done < <(find "$root_abs" -type d -name .git -prune -print -o -type f -name .git -print 2>/dev/null || true)
}

discover_nested_repo_paths_cached_to_file() {
  local root="$1"
  local output_file="$2"
  local root_abs=""
  local repos_json=""
  local repo_line=""
  local repo_path=""
  local used_mode=""
  local refreshed=0
  local stats_file=""
  local -a registered_paths=()
  local -a unregistered_paths=()
  local -a cached_registered_paths=()

  root_abs="$(cd "$root" && pwd -P)"

  stats_file="$(mktemp 2>/dev/null || true)"
  repos_json="$(GITH_DISCOVER_STATS_FILE="$stats_file" GITH_DISCOVER_QUIET=1 GITH_DISCOVER_CACHE=1 GITH_DISCOVER_INCREMENTAL=1 GITH_DISCOVER_METADATA_LEVEL=minimal gith_discover_repos "$root_abs" "6" 2>/dev/null || true)"
  used_mode="$(sed -n 's/^mode=//p' "$stats_file" 2>/dev/null | head -n1)"
  [[ -n "$used_mode" ]] || used_mode="unknown"
  DISCOVER_MODE_LAST="$used_mode"

  if [[ "$repos_json" != \[*\] ]]; then
    discover_nested_repo_paths_fullscan "$root_abs" > "$output_file"
    DISCOVER_MODE_LAST="scan-miss"
    return 0
  fi

  while IFS= read -r repo_path; do
    [[ -n "$repo_path" ]] || continue
    registered_paths+=("$repo_path")
  done < <(collect_registered_repo_paths_recursive "$root_abs")

  while IFS= read -r repo_line; do
    local repo_type=""
    [[ -n "$repo_line" ]] || continue
    repo_path="$(printf '%s' "$repo_line" | sed -n 's/^.*"path":"\([^"]*\)".*$/\1/p')"
    repo_type="$(printf '%s' "$repo_line" | sed -n 's/^.*"type":"\([^"]*\)".*$/\1/p')"
    [[ -n "$repo_path" ]] || continue
    repo_path="$(printf '%b' "${repo_path//\\//}")"
    if [[ "$repo_path" != /* ]]; then
      repo_path="$root_abs/$repo_path"
    fi
    repo_path="$(cd "$repo_path" 2>/dev/null && pwd -P || true)"
    [[ -n "$repo_path" ]] || continue
    [[ "$repo_path" == "$root_abs" ]] && continue
    if [[ "$repo_type" == "registered" ]]; then
      cached_registered_paths+=("$repo_path")
      continue
    fi
    if [[ "$repo_type" == "unregistered" ]]; then
      unregistered_paths+=("$repo_path")
    fi
  done < <(printf '%s' "$repos_json" | grep -o '{[^}]*}')

  if ! validate_unregistered_cache_entries unregistered_paths registered_paths cached_registered_paths; then
    repos_json="$(GITH_DISCOVER_STATS_FILE="$stats_file" GITH_DISCOVER_QUIET=1 GITH_DISCOVER_CACHE=1 GITH_DISCOVER_CACHE_BUST=1 GITH_DISCOVER_INCREMENTAL=1 GITH_DISCOVER_METADATA_LEVEL=minimal gith_discover_repos "$root_abs" "6" 2>/dev/null || true)"
    refreshed=1
    unregistered_paths=()
    cached_registered_paths=()
    while IFS= read -r repo_line; do
      local repo_type=""
      [[ -n "$repo_line" ]] || continue
      repo_path="$(printf '%s' "$repo_line" | sed -n 's/^.*"path":"\([^"]*\)".*$/\1/p')"
      repo_type="$(printf '%s' "$repo_line" | sed -n 's/^.*"type":"\([^"]*\)".*$/\1/p')"
      [[ -n "$repo_path" ]] || continue
      repo_path="$(printf '%b' "${repo_path//\\//}")"
      if [[ "$repo_path" != /* ]]; then
        repo_path="$root_abs/$repo_path"
      fi
      repo_path="$(cd "$repo_path" 2>/dev/null && pwd -P || true)"
      [[ -n "$repo_path" ]] || continue
      [[ "$repo_path" == "$root_abs" ]] && continue
      if [[ "$repo_type" == "registered" ]]; then
        cached_registered_paths+=("$repo_path")
        continue
      fi
      if [[ "$repo_type" == "unregistered" ]]; then
        unregistered_paths+=("$repo_path")
      fi
    done < <(printf '%s' "$repos_json" | grep -o '{[^}]*}')
  fi

  while IFS= read -r repo_path; do
    [[ -n "$repo_path" ]] || continue
    printf '%s\n' "$repo_path"
  done < <(printf '%s\n' "${registered_paths[@]}" "${unregistered_paths[@]}" | awk 'NF' | sort -u) > "$output_file"

  if [[ "$refreshed" -eq 1 ]]; then
    DISCOVER_MODE_LAST="cache-refresh"
    echo "INFO: Refreshed discover cache due to unregistered repo cache inconsistency" >&2
  fi

  rm -f "$stats_file" 2>/dev/null || true

  return 0
}

collect_registered_repo_paths_recursive() {
  local root="$1"
  local -a queue=()
  local -a seen=()
  local current=""
  local sub_path=""
  local child_abs=""

  queue+=("$(cd "$root" && pwd -P)")

  while [[ ${#queue[@]} -gt 0 ]]; do
    current="${queue[0]}"
    queue=("${queue[@]:1}")

    [[ -f "$current/.gitmodules" ]] || continue

    while IFS= read -r sub_path; do
      [[ -n "$sub_path" ]] || continue
      child_abs="$(cd "$current/$sub_path" 2>/dev/null && pwd -P || true)"
      [[ -n "$child_abs" ]] || continue

      if ! printf '%s\n' "${seen[@]}" | grep -Fxq "$child_abs"; then
        seen+=("$child_abs")
        queue+=("$child_abs")
        printf '%s\n' "$child_abs"
      fi
    done < <(git config -f "$current/.gitmodules" --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk '{print $2}' || true)
  done
}

validate_unregistered_cache_entries() {
  local -n unregistered_ref="$1"
  local -n registered_ref="$2"
  local -n cached_registered_ref="$3"
  local candidate=""

  for candidate in "${unregistered_ref[@]}"; do
    [[ -n "$candidate" ]] || continue
    if ! git -C "$candidate" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      return 1
    fi
    if printf '%s\n' "${registered_ref[@]}" | grep -Fxq "$candidate"; then
      return 1
    fi
  done

  for candidate in "${cached_registered_ref[@]}"; do
    [[ -n "$candidate" ]] || continue
    if ! printf '%s\n' "${registered_ref[@]}" | grep -Fxq "$candidate"; then
      return 1
    fi
  done

  for candidate in "${registered_ref[@]}"; do
    [[ -n "$candidate" ]] || continue
    if ! printf '%s\n' "${cached_registered_ref[@]}" | grep -Fxq "$candidate"; then
      return 1
    fi
  done

  return 0
}

sync_single_repo_branch_mode() {
  local repo="$1"
  local workspace_root="$2"
  local preferred_remote="$3"
  local is_root="$4"
  local dry_run="$5"

  local repo_abs=""
  local remote_name=""
  local current_branch=""
  local default_branch=""
  local target_branch=""
  local branch_source=""
  local gm_file=""
  local branch_from_gitmodules=""
  local is_registered=0
  local rel=""
  local checkout_cmd=()
  local pull_cmd=()

  repo_abs="$(cd "$repo" && pwd -P)"
  remote_name="$(resolve_repo_remote "$repo_abs" "$preferred_remote" || true)"
  if [[ -z "$remote_name" ]]; then
    echo "WARN: Skip repo without remotes: $repo_abs" >&2
    return 0
  fi

  git -C "$repo_abs" fetch "$remote_name" --prune --tags >/dev/null 2>&1 || true
  current_branch="$(git -C "$repo_abs" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"

  if [[ "$is_root" -eq 1 ]]; then
    if [[ -n "$current_branch" ]]; then
      target_branch="$current_branch"
      branch_source="root current branch"
    else
      default_branch="$(detect_remote_default_branch "$repo_abs" "$remote_name" || true)"
      [[ -z "$default_branch" ]] && echo "ERROR: Could not detect default branch for root repo remote '$remote_name'" >&2 && return 1
      target_branch="$default_branch"
      branch_source="root detached -> remote default"
    fi
  else
    gm_file="$(syncc_find_gitmodules_file_for_path "$workspace_root" "$repo_abs" || true)"
    if [[ -n "$gm_file" ]]; then
      is_registered=1
      branch_from_gitmodules="$(syncc_get_gitmodules_branch_for_path "$workspace_root" "$repo_abs" || true)"
      if [[ -n "$branch_from_gitmodules" ]]; then
        target_branch="$branch_from_gitmodules"
        branch_source="registered .gitmodules branch"
      else
        default_branch="$(detect_remote_default_branch "$repo_abs" "$remote_name" || true)"
        [[ -z "$default_branch" ]] && echo "ERROR: Could not detect default branch for registered repo: $repo_abs" >&2 && return 1
        target_branch="$default_branch"
        branch_source="registered remote default branch"
      fi
    else
      if [[ -n "$current_branch" ]]; then
        target_branch="$current_branch"
        branch_source="unregistered current branch"
      else
        default_branch="$(detect_remote_default_branch "$repo_abs" "$remote_name" || true)"
        [[ -z "$default_branch" ]] && echo "ERROR: Could not detect default branch for detached unregistered repo: $repo_abs" >&2 && return 1
        target_branch="$default_branch"
        branch_source="unregistered detached -> remote default"
      fi
    fi
  fi

  if [[ -z "$target_branch" ]]; then
    echo "ERROR: Could not determine target branch for repo: $repo_abs" >&2
    return 1
  fi

  rel="${repo_abs#$workspace_root/}"
  [[ "$repo_abs" == "$workspace_root" ]] && rel="."

  if git -C "$repo_abs" show-ref --verify --quiet "refs/heads/$target_branch"; then
    checkout_cmd=(git -C "$repo_abs" checkout "$target_branch")
  elif git -C "$repo_abs" show-ref --verify --quiet "refs/remotes/$remote_name/$target_branch"; then
    checkout_cmd=(git -C "$repo_abs" checkout -b "$target_branch" "$remote_name/$target_branch")
  else
    if [[ "$is_registered" -eq 1 || "$is_root" -eq 1 ]]; then
      echo "ERROR: Target branch '$target_branch' not found for $rel ($branch_source)" >&2
      return 1
    fi
    echo "WARN: Unregistered repo branch '$target_branch' has no remote ref on '$remote_name'; keep local branch only: $rel"
    checkout_cmd=(git -C "$repo_abs" checkout "$target_branch")
  fi

  pull_cmd=(git -C "$repo_abs" pull --rebase "$remote_name" "$target_branch")

  if [[ "$dry_run" -eq 1 ]]; then
    echo "[DRY RUN] Repo: $rel"
    echo "[DRY RUN] Branch source: $branch_source"
    echo "[DRY RUN] Would run: ${checkout_cmd[*]}"
    if git -C "$repo_abs" show-ref --verify --quiet "refs/remotes/$remote_name/$target_branch"; then
      echo "[DRY RUN] Would run: ${pull_cmd[*]}"
    else
      echo "[DRY RUN] Skip pull: missing remote branch $remote_name/$target_branch"
    fi
    return 0
  fi

  "${checkout_cmd[@]}" >/dev/null 2>&1

  if git -C "$repo_abs" show-ref --verify --quiet "refs/remotes/$remote_name/$target_branch"; then
    "${pull_cmd[@]}" >/dev/null 2>&1 || true
  fi
}

sync_workspace_repos_branch_mode() {
  local root_repo="$1"
  local preferred_remote="$2"
  local dry_run="$3"
  local root_abs=""
  local repo_path=""
  local discover_mode="unknown"
  local repo_list_file=""

  root_abs="$(cd "$root_repo" && pwd -P)"

  sync_single_repo_branch_mode "$root_abs" "$root_abs" "$preferred_remote" 1 "$dry_run"
  sync_submodules_after_sync "$root_abs" "$dry_run"

  repo_list_file="$(mktemp 2>/dev/null || true)"
  if [[ -z "$repo_list_file" ]]; then
    repo_list_file="$root_abs/.git/.kano-cache/discover-repos/.sync-repo-list-$$.tmp"
  fi

  discover_nested_repo_paths_cached_to_file "$root_abs" "$repo_list_file"
  discover_mode="$DISCOVER_MODE_LAST"

  while IFS= read -r repo_path; do
    [[ -n "$repo_path" ]] || continue
    sync_single_repo_branch_mode "$repo_path" "$root_abs" "$preferred_remote" 0 "$dry_run"
    sync_submodules_after_sync "$repo_path" "$dry_run"
  done < "$repo_list_file"

  rm -f "$repo_list_file" 2>/dev/null || true

  echo "Discover mode: $discover_mode"
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
  local repo_abs=""
  local gm=""
  repo_abs="$(cd "$repo" && pwd -P)"
  gm="$repo_abs/.gitmodules"
  [[ -f "$gm" ]] || return 0
  git config -f "$gm" --get-regexp '^submodule\..*\.path$' 2>/dev/null | awk '{print $2}' || true
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
  local stale_list=""

  gm_paths="$(collect_direct_submodule_paths "$repo")"
  gitlinks="$(collect_index_gitlink_paths "$repo")"

  # If .gitmodules exists but cannot be parsed into paths, skip cleanup to avoid destructive false positives.
  if [[ -f "$repo/.gitmodules" ]] && [[ -z "$gm_paths" ]]; then
    echo "WARN: Skip stale submodule gitlink cleanup; failed to parse .gitmodules paths in $repo" >&2
    return 0
  fi

  while IFS= read -r p; do
    [[ -n "$p" ]] || continue
    if ! grep -Fxq "$p" <<<"$gm_paths"; then
      stale_list+="$p"$'\n'
    fi
  done <<<"$gitlinks"

  while IFS= read -r p; do
    [[ -n "$p" ]] || continue
    echo "Repairing stale submodule gitlink: $p"
    git -C "$repo" submodule deinit -- "$p" >/dev/null 2>&1 || true
    git -C "$repo" update-index --force-remove -- "$p"
    had_fix=1
  done <<<"$stale_list"

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

sync_unregistered_repos_to_branches() {
  local repo="$1"
  local dry_run="$2"
  local registered_paths=()
  local all_nested=()
  local path=""
  local rel=""

  # Collect registered submodule paths.
  while IFS= read -r path; do
    [[ -n "$path" ]] && registered_paths+=("$repo/$path")
  done < <(collect_direct_submodule_paths "$repo")

  # Discover all nested git repos via filesystem scan.
  while IFS= read -r git_marker; do
    [[ -z "$git_marker" ]] && continue
    path="$(dirname "$git_marker")"
    if [[ "$path" != "$repo" ]]; then
      all_nested+=("$path")
    fi
  done < <(find "$repo" -type d -name .git -prune -print -o -type f -name .git -print 2>/dev/null || true)

  # Identify unregistered (not in registered_paths).
  for path in "${all_nested[@]}"; do
    local is_registered=0
    for reg in "${registered_paths[@]}"; do
      if [[ "$path" == "$reg" ]]; then
        is_registered=1
        break
      fi
    done
    if [[ "$is_registered" -eq 1 ]]; then
      continue
    fi

    if ! git -C "$path" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
      continue
    fi

    local current_branch=""
    current_branch="$(git -C "$path" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
    if [[ -n "$current_branch" ]]; then
      continue
    fi

    # Detached HEAD found in unregistered repo.
    local remote_name="origin"
    if ! git -C "$path" remote get-url "$remote_name" >/dev/null 2>&1; then
      remote_name="upstream"
    fi
    if ! git -C "$path" remote get-url "$remote_name" >/dev/null 2>&1; then
      remote_name="$(git -C "$path" remote | head -n 1 || true)"
    fi
    if [[ -z "$remote_name" ]]; then
      continue
    fi

    local default_branch=""
    default_branch="$(detect_remote_default_branch "$path" "$remote_name" || true)"
    if [[ -z "$default_branch" ]]; then
      continue
    fi

    local head_sha=""
    local default_sha=""
    head_sha="$(git -C "$path" rev-parse HEAD 2>/dev/null || true)"
    default_sha="$(git -C "$path" rev-parse "$remote_name/$default_branch" 2>/dev/null || true)"

    if [[ "$head_sha" != "$default_sha" ]]; then
      continue
    fi

    rel="${path#$repo/}"
    if [[ "$dry_run" -eq 1 ]]; then
      echo "[DRY RUN] Would attach unregistered repo detached HEAD to $default_branch: $rel"
    else
      echo "Attaching unregistered repo detached HEAD to $default_branch: $rel"
      if git -C "$path" show-ref --verify --quiet "refs/heads/$default_branch"; then
        git -C "$path" checkout "$default_branch" >/dev/null 2>&1 || true
      else
        git -C "$path" checkout -b "$default_branch" "$remote_name/$default_branch" >/dev/null 2>&1 || true
      fi
    fi
  done
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
  echo "Syncing workspace repos with recursive branch rules"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    sync_workspace_repos_branch_mode "$REPO" "$REMOTE" "$DRY_RUN"
    exit 0
  fi

  if ! sync_workspace_repos_branch_mode "$REPO" "$REMOTE" "$DRY_RUN"; then
    if [[ "$HAD_STASH" -eq 1 ]]; then
      echo "Auto-stash kept due to sync failure. Recover with: git -C \"<repo>\" stash list" >&2
    fi
    echo "Sync failed while applying recursive branch rules." >&2
    exit 1
  fi

  echo "=== Sync Complete ==="
  echo "Root branch: $(git -C "$REPO" symbolic-ref --quiet --short HEAD 2>/dev/null || echo detached)"
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
