#!/usr/bin/env bash
#
# smart-commit-with-ai-review.sh - Project-level wrapper for commits with AI review enabled (repo-passive-mode profile)

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
if ! has_arg "--ai-review" "${ARGS[@]}" && ! has_arg "--no-ai-review" "${ARGS[@]}"; then
  ARGS+=("--ai-review")
fi

export KANO_GIT_MASTER_ROOT="$ROOT"
set +e
bash "$SKILL_SCRIPT" "${ARGS[@]}"
status=$?
set -e
pause_if_needed "$@"
exit "$status"
