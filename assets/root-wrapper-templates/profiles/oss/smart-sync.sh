#!/usr/bin/env bash
#
# smart-sync.sh - Project-level entry point for sync workflows (oss profile)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ORIG_ARGS=("$@")
source "$ROOT/smart-wrapper-common.sh"
GIT_MASTER_SKILL_SCRIPT_DIR=".agents/kano/kano-git-master-skill/scripts/commit-tools/sync"

usage() {
  cat <<'USAGE'
Usage: ./smart-sync.sh <mode> [args...]

Modes:
  upstream-stable-dev  Stable maintenance branch migration and sync (recommended for release updates)
  stable-dev           Alias of upstream-stable-dev
  dev                  Base on upstream default branch and migrate maintenance commits
  upstream-force-push  Sync with upstream, then push --force-with-lease to origin
  origin-latest        Checkout origin default branch and pull --rebase (no push)

Examples:
  ./smart-sync.sh origin-latest
  ./smart-sync.sh upstream-stable-dev --target-tag v1.0.0 --base-tag v0.9.9
  ./smart-sync.sh dev
  ./smart-sync.sh upstream-force-push --verbose
USAGE
}

run_skill_script() {
  local script_rel="$1"
  shift || true
  local script="$ROOT/$GIT_MASTER_SKILL_SCRIPT_DIR/$script_rel"
  ensure_skill_script_exists "$script"
  export KANO_GIT_MASTER_ROOT="$ROOT"
  bash "$script" "$@"
}

ensure_git_master_skill_ready() {
  local skill_script_dir="$ROOT/$GIT_MASTER_SKILL_SCRIPT_DIR"
  if [[ -d "$skill_script_dir" ]]; then
    return 0
  fi

  echo "[smart-sync] kano-git-master-skill not found, bootstrapping submodules..." >&2
  (
    cd "$ROOT"
    git submodule init
    git submodule sync --recursive
    git submodule update --init --recursive
  )

  if [[ ! -d "$skill_script_dir" ]]; then
    echo "ERROR: kano-git-master-skill is still unavailable after submodule bootstrap." >&2
    echo "Expected directory: $skill_script_dir" >&2
    exit 1
  fi
}

mode="${1:-}"
if [[ -z "$mode" || "$mode" == "-h" || "$mode" == "--help" ]]; then
  usage
  exit 0
fi
shift || true

case "$mode" in
  upstream-stable-dev|stable-dev)
    set +e
    bash "$ROOT/smart-sync-upstream-stable-dev.sh" "$@"
    status=$?
    set -e
    ;;
  upstream-force-push)
    ensure_git_master_skill_ready
    set +e
    run_skill_script "smart-sync-upstream-force-push-copilot.sh" "$@"
    status=$?
    set -e
    ;;
  origin-latest)
    ensure_git_master_skill_ready
    set +e
    run_skill_script "smart-sync-origin-latest.sh" "$@"
    status=$?
    set -e
    ;;
  dev)
    ensure_git_master_skill_ready
    set +e
    run_skill_script "smart-sync-dev.sh" "$@"
    status=$?
    set -e
    ;;
  *)
    echo "ERROR: Unknown mode: $mode" >&2
    usage >&2
    exit 1
    ;;
esac

pause_if_needed "${ORIG_ARGS[@]}"
exit "${status:-0}"
