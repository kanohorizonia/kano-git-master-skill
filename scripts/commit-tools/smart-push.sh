#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DRY_RUN=0
FORCE_WITH_LEASE=0
INCLUDE_ROOT=1
INCLUDE_SUBMODULES=1
INCLUDE_STANDALONE=1
REPO_FILTER=""

usage() {
  cat <<'EOF'
Usage: smart-push.sh [options]

Push workflow with SSH→HTTP fallback per repo.

Options:
  --repos <paths>        Only process specific repos (comma-separated)
  --no-root              Exclude root repo
  --no-submodules        Exclude submodules
  --no-standalone        Exclude standalone repos
  --force-with-lease     Use --force-with-lease on push
  --dry-run              Show what would be done without doing it
  -h, --help             Show help

Examples:
  smart-push.sh
  smart-push.sh --repos ".,skills/kano"
  smart-push.sh --no-standalone
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
    --force-with-lease)
      FORCE_WITH_LEASE=1
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

ROOT="$(cd "$SCRIPT_DIR/../.." && git rev-parse --show-toplevel 2>/dev/null || true)"
if [[ -z "$ROOT" ]]; then
  ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
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
while IFS= read -r repo; do
  [[ -z "$repo" ]] && continue
  path="$(echo "$repo" | grep -o '"path":"[^"]*"' | sed 's/"path":"//;s/"$//')"
  [[ -z "$path" ]] && continue
  REPOS+=("$path")
done < <(echo "$repos_json" | grep -o '{[^}]*}')

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

for repo in "${REPOS[@]}"; do
  echo ""
  echo "=== Push: $repo ==="

  branch="$(git -C "$repo" symbolic-ref --quiet --short HEAD 2>/dev/null || true)"
  if [[ -z "$branch" ]]; then
    echo "[$repo] SKIP: Detached HEAD"
    continue
  fi

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would push $branch"
    continue
  fi

  set_upstream=0
  if ! git -C "$repo" rev-parse --abbrev-ref "@{upstream}" >/dev/null 2>&1; then
    set_upstream=1
  fi

  push_args=()
  if [[ "$FORCE_WITH_LEASE" -eq 1 ]]; then
    push_args+=("--force-with-lease")
  fi
  if [[ "$set_upstream" -eq 1 ]]; then
    push_args+=("-u")
  fi

  primary_remote=""
  fallback_remote=""

  if git -C "$repo" remote get-url origin-ssh >/dev/null 2>&1; then
    primary_remote="origin-ssh"
    if git -C "$repo" remote get-url origin-http >/dev/null 2>&1; then
      fallback_remote="origin-http"
    fi
  elif git -C "$repo" remote get-url origin-http >/dev/null 2>&1; then
    primary_remote="origin-http"
  elif git -C "$repo" remote get-url origin >/dev/null 2>&1; then
    primary_remote="origin"
  else
    echo "[$repo] ERROR: No origin remote found" >&2
    FAILED=1
    continue
  fi

  echo "[$repo] Pushing to $primary_remote..."
  if git -C "$repo" push "${push_args[@]}" "$primary_remote" "$branch"; then
    echo "[$repo] Push successful"
    continue
  fi

  if [[ -n "$fallback_remote" ]]; then
    echo "[$repo] Falling back to $fallback_remote..."
    if git -C "$repo" push "${push_args[@]}" "$fallback_remote" "$branch"; then
      echo "[$repo] Push successful"
      continue
    fi
  fi

  echo "[$repo] ERROR: Push failed" >&2
  FAILED=1
done

if [[ "$FAILED" -eq 1 ]]; then
  exit 1
fi
