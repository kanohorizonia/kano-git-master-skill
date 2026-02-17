#!/usr/bin/env bash
#
# smart-sync-upstream-force-push.sh - Sync with upstream then force-push to origin
#
# Purpose:
#   Typical fork workflow:
#     1) Rebase current branch onto upstream's default branch (AI-powered)
#     2) Push rewritten history back to origin with --force-with-lease
#
# Usage:
#   ./smart-sync-upstream-force-push.sh --provider <name> --model <name> [options]
#
# Required:
#   --provider <name>           AI provider (opencode, codex, copilot)
#   --model <name>              AI model name
#
# Options:
#   --origin-remote <name>      Push remote (default: origin)
#   --upstream-remote <name>    Sync remote (default: upstream)
#   --onto <ref>                Override sync target (default: upstream/<default-branch>)
#   --no-verify                 Pass --no-verify to git push
#   --dry-run                   Show what would be done
#   -h, --help                  Show help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../lib/git-helpers.sh"

AI_PROVIDER=""
AI_MODEL=""
ORIGIN_REMOTE="origin"
UPSTREAM_REMOTE="upstream"
ONTO_REF=""
NO_VERIFY=0
DRY_RUN=0
REPO="."

usage() {
  cat <<'EOF'
Usage: smart-sync-upstream-force-push.sh --provider <name> --model <name> [options]

Sync with upstream then push --force-with-lease to origin.

Required:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Options:
  --origin-remote <name>      Push remote (default: origin)
  --upstream-remote <name>    Sync remote (default: upstream)
  --onto <ref>                Override sync target (default: upstream/<default-branch>)
  --no-verify                 Pass --no-verify to git push
  --dry-run                   Show what would be done
  -h, --help                  Show help
EOF
}

passthrough_args=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --provider)
      AI_PROVIDER="${2:-}"
      shift 2
      ;;
    --model)
      AI_MODEL="${2:-}"
      shift 2
      ;;
    --origin-remote)
      ORIGIN_REMOTE="${2:-}"
      shift 2
      ;;
    --upstream-remote)
      UPSTREAM_REMOTE="${2:-}"
      shift 2
      ;;
    --onto)
      ONTO_REF="${2:-}"
      shift 2
      ;;
    --no-verify)
      NO_VERIFY=1
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
      passthrough_args+=("$1")
      shift
      ;;
  esac
done

if [[ -z "$AI_PROVIDER" || -z "$AI_MODEL" ]]; then
  echo "ERROR: --provider and --model are required" >&2
  usage >&2
  exit 1
fi

if ! validate_repo "$REPO"; then
  exit 1
fi

if ! is_clean_working_tree "$REPO"; then
  echo "ERROR: Working tree has uncommitted changes" >&2
  echo "Commit or stash changes before syncing" >&2
  exit 1
fi

if ! git -C "$REPO" remote get-url "$ORIGIN_REMOTE" >/dev/null 2>&1; then
  echo "ERROR: Origin remote not found: $ORIGIN_REMOTE" >&2
  exit 1
fi

if ! git -C "$REPO" remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
  echo "ERROR: Upstream remote not found: $UPSTREAM_REMOTE" >&2
  exit 1
fi

current_branch="$(get_current_branch "$REPO")"
if [[ -z "$current_branch" ]]; then
  echo "ERROR: Detached HEAD state" >&2
  exit 1
fi

git -C "$REPO" fetch "$UPSTREAM_REMOTE" --prune >/dev/null 2>&1 || true
git -C "$REPO" fetch "$ORIGIN_REMOTE" --prune >/dev/null 2>&1 || true

if [[ -z "$ONTO_REF" ]]; then
  upstream_default="$(gith_get_default_branch "$UPSTREAM_REMOTE" "$REPO" || true)"
  if [[ -z "${upstream_default:-}" ]]; then
    echo "ERROR: Could not detect default branch for remote: $UPSTREAM_REMOTE" >&2
    exit 1
  fi
  ONTO_REF="$UPSTREAM_REMOTE/$upstream_default"
fi

sync_cmd=("$SCRIPT_DIR/smart-sync.sh" --provider "$AI_PROVIDER" --model "$AI_MODEL" --onto "$ONTO_REF")
if [[ "$DRY_RUN" -eq 1 ]]; then
  sync_cmd+=("--dry-run")
fi
sync_cmd+=("${passthrough_args[@]}")

push_cmd=(git -C "$REPO" push --force-with-lease "$ORIGIN_REMOTE" "$current_branch")
if [[ "$NO_VERIFY" -eq 1 ]]; then
  push_cmd=(git -C "$REPO" push --no-verify --force-with-lease "$ORIGIN_REMOTE" "$current_branch")
fi

echo "Syncing $current_branch → $ONTO_REF"
if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[DRY RUN] Would run: ${sync_cmd[*]}"
  echo "[DRY RUN] Would run: ${push_cmd[*]}"
  exit 0
fi

bash "${sync_cmd[@]}"

echo "Force-pushing to $ORIGIN_REMOTE/$current_branch (with --force-with-lease)"
"${push_cmd[@]}"

echo "=== Done ==="

