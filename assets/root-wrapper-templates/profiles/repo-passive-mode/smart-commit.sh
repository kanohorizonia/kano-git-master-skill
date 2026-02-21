#!/usr/bin/env bash
#
# smart-commit.sh - Project-level wrapper for AI-powered commits (repo-passive-mode profile)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT/smart-wrapper-common.sh"

SKILL_SCRIPT="$(resolve_skill_script_path "$ROOT" "scripts/commit-tools/commit/smart-commit-copilot.sh")"
ensure_skill_script_exists "$SKILL_SCRIPT"

ARGS=("$@")
if ! has_arg "--repos" "${ARGS[@]}"; then
  REPOS_CSV="$(collect_cloned_repos_csv "$ROOT")"
  ARGS+=("--repos" "$REPOS_CSV")
fi

export KANO_GIT_MASTER_ROOT="$ROOT"
set +e
bash "$SKILL_SCRIPT" "${ARGS[@]}"
status=$?
set -e
pause_if_needed "$@"
exit "$status"
