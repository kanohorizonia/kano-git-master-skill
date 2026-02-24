#!/usr/bin/env bash
#
# smart-sync.sh - Project-level entry point for sync workflows (no-ai-review profile)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORIG_ARGS=("$@")
source "$ROOT/smart-wrapper-common.sh"

resolve_sync_script_dir() {
  local skill_root
  if skill_root="$(resolve_git_master_skill_root "$ROOT" 2>/dev/null)"; then
    printf '%s/scripts/commit-tools/sync' "$skill_root"
    return 0
  fi
  printf '%s/.agents/skills/kano/kano-git-master-skill/scripts/commit-tools/sync' "$ROOT"
}

usage() {
  cat <<'USAGE'
Usage: ./smart-sync.sh [origin-latest] [args...]

Examples:
  ./smart-sync.sh
  ./smart-sync.sh origin-latest

Behavior:
  - Passive mode for multi-device repository usage.
  - Only sync root repo and submodules that are already cloned locally.
  - Never auto-initialize or clone missing submodules.
USAGE
}

run_skill_script() {
  local script_rel="$1"
  shift || true
  local script_dir
  script_dir="$(resolve_sync_script_dir)"
  local script="$script_dir/$script_rel"
  ensure_skill_script_exists "$script"
  export KANO_GIT_MASTER_ROOT="$ROOT"
  bash "$script" "$@"
}

first_arg="${1:-}"
if [[ "$first_arg" == "-h" || "$first_arg" == "--help" ]]; then
  usage
  exit 0
fi

if [[ "$first_arg" == "origin-latest" ]]; then
  shift
elif [[ -n "$first_arg" ]]; then
  case "$first_arg" in
    upstream-stable-dev|stable-dev|dev|upstream-force-push)
      echo "ERROR: '$first_arg' is an advanced mode and is not handled by ./smart-sync.sh." >&2
      usage >&2
      exit 1
      ;;
  esac
fi

REPOS_CSV="$(collect_cloned_repos_csv "$ROOT")"
IFS=',' read -r -a REPOS <<< "$REPOS_CSV"
FAILED=0

for repo in "${REPOS[@]}"; do
  [[ -z "$repo" ]] && continue
  echo "[smart-sync] syncing repo: $repo"
  if ! run_skill_script "smart-sync-origin-latest.sh" --repo "$repo" "$@"; then
    FAILED=1
  fi
done

if [[ "$FAILED" -ne 0 ]]; then
  pause_if_needed "${ORIG_ARGS[@]}"
  exit 1
fi

pause_if_needed "${ORIG_ARGS[@]}"
