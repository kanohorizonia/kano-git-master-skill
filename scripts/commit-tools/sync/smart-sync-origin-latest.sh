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
  --dry-run           Show what would be done
  -h, --help          Show help

Examples:
  ./smart-sync-origin-latest.sh
  ./smart-sync-origin-latest.sh --remote origin
  ./smart-sync-origin-latest.sh --target release
  ./smart-sync-origin-latest.sh --target release --release-channel any
EOF
}

find_latest_release_tag() {
  local repo="$1"
  local pattern="$2"

  git -C "$repo" tag --list --sort=-version:refname \
    | grep -Ei "$pattern" \
    | head -n1 || true
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
  echo "ERROR: Working tree has uncommitted changes" >&2
  echo "Commit or stash changes before syncing" >&2
  exit 1
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
  "${pull_cmd[@]}"

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
  echo "=== Sync Complete ==="
  echo "On release tag: $latest_tag (detached HEAD)"
fi
