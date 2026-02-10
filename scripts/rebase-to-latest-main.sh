#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
fi

BRANCH="main"
DETACHED_MODE="checkout" # checkout|skip
AUTO_STASH=1

usage() {
  cat <<'EOF'
Usage: rebase-to-latest-main.sh [options]

Rebase the current local branch onto the latest `origin/main` across:
  1) root repo
  2) all submodules (recursive)

Default behavior:
  - Fetch `origin/<branch>` (default: main)
  - If HEAD is on a branch: `git rebase origin/<branch>`
  - If HEAD is detached: checkout `<branch>` at `origin/<branch>` (default)
  - Auto-stash local changes (including untracked) before operations, pop after success
  - Does NOT push

Options:
  --branch <name>              Base branch name (default: main)
  --detached <checkout|skip>   What to do when HEAD is detached (default: checkout)
  --no-stash                   Fail if there are local changes (no auto-stash)
  -h, --help                   Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --branch)
      BRANCH="${2:-}"
      shift 2
      ;;
    --detached)
      DETACHED_MODE="${2:-}"
      shift 2
      ;;
    --no-stash)
      AUTO_STASH=0
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

if [[ -z "$BRANCH" ]]; then
  echo "ERROR: --branch requires a value." >&2
  exit 2
fi

if [[ "$DETACHED_MODE" != "checkout" && "$DETACHED_MODE" != "skip" ]]; then
  echo "ERROR: --detached must be 'checkout' or 'skip'." >&2
  exit 2
fi

if ! git -C "$ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ERROR: not a git repository: $ROOT" >&2
  exit 1
fi

declare -a REPOS=()
REPO_LIST_FILE="$(mktemp -t rebase-to-latest-main.repos.XXXXXX)"
trap 'rm -f "$REPO_LIST_FILE"' EXIT
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

add_repo "$ROOT"

if [[ -f "$ROOT/.gitmodules" ]]; then
  while IFS= read -r sm_path; do
    [[ -n "$sm_path" ]] || continue
    add_repo "$ROOT/$sm_path"
  done < <(git -C "$ROOT" submodule foreach --recursive --quiet 'echo "$sm_path"' 2>/dev/null || true)
fi

process_repo() {
  local repo="$1"
  local stash_created=0
  local stash_ref=""

  echo "==> [$repo]"

  if ! git -C "$repo" remote get-url origin >/dev/null 2>&1; then
    echo "Skip: 'origin' remote not found."
    return 0
  fi

  echo "Fetching 'origin/$BRANCH'..."
  git -C "$repo" fetch --prune origin "$BRANCH"

  if ! git -C "$repo" show-ref --verify --quiet "refs/remotes/origin/$BRANCH"; then
    echo "Skip: 'origin/$BRANCH' does not exist."
    return 0
  fi

  if [[ -n "$(git -C "$repo" status --porcelain)" ]]; then
    if [[ "$AUTO_STASH" -eq 0 ]]; then
      echo "ERROR: local changes detected; re-run without --no-stash or clean the working tree." >&2
      return 1
    fi
    echo "Local changes detected; stashing before rebase..."
    git -C "$repo" stash push -u -m "auto-stash: rebase-to-latest-main"
    stash_created=1
    stash_ref="$(git -C "$repo" stash list -n 1 --format='%gd')"
  fi

  local start_branch=""
  start_branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD || true)"
  if [[ -z "$start_branch" ]]; then
    if [[ "$DETACHED_MODE" == "skip" ]]; then
      echo "Skip: HEAD is detached (use --detached checkout to update)."
      return 0
    fi
    echo "HEAD is detached; checking out '$BRANCH' at 'origin/$BRANCH'..."
    git -C "$repo" checkout -B "$BRANCH" "refs/remotes/origin/$BRANCH"
  else
    echo "Rebasing '$start_branch' onto 'origin/$BRANCH'..."
    git -C "$repo" rebase "refs/remotes/origin/$BRANCH"
  fi

  if [[ "$stash_created" -eq 1 ]]; then
    echo "Restoring stashed local changes..."
    if ! git -C "$repo" stash pop --index; then
      echo "Warning: auto stash pop failed. Resolve conflicts and apply manually from $stash_ref." >&2
      return 1
    fi
  fi

  return 0
}

echo "Root: $ROOT"
echo "Repos: ${#REPOS[@]} (root + submodules)"

failures=0
for repo in "${REPOS[@]}"; do
  if ! process_repo "$repo"; then
    failures=$((failures + 1))
  fi
done

if [[ "$failures" -ne 0 ]]; then
  echo "ERROR: $failures repo(s) failed. Fix conflicts/errors above, then re-run." >&2
  exit 1
fi

echo "Done."
