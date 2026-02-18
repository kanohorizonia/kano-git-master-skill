#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source "$SCRIPT_DIR/submodule-common.sh"

ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
DRY_RUN=0
INIT_MISSING=0
SYNC_RECURSIVE=1
UPDATE_ORIGIN=1

usage() {
  cat <<'EOF'
Usage: submodule-sync-urls.sh [options]

When `.gitmodules` changes (e.g. submodule URL updated), this script:
  - syncs submodule URLs into the superproject config (`git submodule sync`)
  - updates each submodule repo's `origin` remote URL to match `.gitmodules`

Options:
  --dry-run         Print actions without changing anything
  --init-missing    Initialize missing submodules before syncing/remotes
  --no-recursive    Do not use --recursive for `git submodule sync`
  --no-origin       Do not update submodule repo `origin` remotes
  -h, --help        Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --init-missing)
      INIT_MISSING=1
      shift
      ;;
    --no-recursive)
      SYNC_RECURSIVE=0
      shift
      ;;
    --no-origin)
      UPDATE_ORIGIN=0
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

if [[ -z "$ROOT" ]]; then
  echo "ERROR: not inside a git repository." >&2
  exit 1
fi

if [[ ! -f "$ROOT/.gitmodules" ]]; then
  echo "Skip: no .gitmodules found at $ROOT"
  exit 0
fi

run() {
  subm_run "$([[ "$DRY_RUN" -eq 1 ]] && echo true || echo false)" "$@"
}

echo "Root: $ROOT"

if [[ "$INIT_MISSING" -eq 1 ]]; then
  echo "Initializing submodules (if needed)..."
  run git -C "$ROOT" submodule update --init --recursive
fi

echo "Syncing submodule URLs into superproject config..."
if [[ "$SYNC_RECURSIVE" -eq 1 ]]; then
  run git -C "$ROOT" submodule sync --recursive
else
  run git -C "$ROOT" submodule sync
fi

if [[ "$UPDATE_ORIGIN" -eq 0 ]]; then
  echo "Done (skipped updating submodule origin remotes)."
  exit 0
fi

echo "Updating submodule repo origin URLs to match .gitmodules..."

while IFS= read -r key; do
  name="${key#submodule.}"
  name="${name%.url}"

  path="$(git -C "$ROOT" config -f "$ROOT/.gitmodules" --get "submodule.$name.path" || true)"
  url="$(git -C "$ROOT" config -f "$ROOT/.gitmodules" --get "submodule.$name.url" || true)"

  if [[ -z "$path" || -z "$url" ]]; then
    echo "Skip: invalid .gitmodules entry for submodule.$name (missing path/url)."
    continue
  fi

  repo="$ROOT/$path"
  if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "Skip: submodule not initialized: $path"
    continue
  fi

  current_url="$(git -C "$repo" remote get-url origin 2>/dev/null || true)"
  if [[ -z "$current_url" ]]; then
    echo "[$path] add origin -> $url"
    run git -C "$repo" remote add origin "$url"
    continue
  fi

  if [[ "$current_url" == "$url" ]]; then
    echo "[$path] origin already up-to-date"
    continue
  fi

  echo "[$path] set origin: $current_url -> $url"
  run git -C "$repo" remote set-url origin "$url"
done < <(git -C "$ROOT" config -f "$ROOT/.gitmodules" --name-only --get-regexp '^submodule\..*\.url$' || true)

echo "Done."
