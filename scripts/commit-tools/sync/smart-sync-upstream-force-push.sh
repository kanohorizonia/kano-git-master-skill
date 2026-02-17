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
#   --repo <path>              Target repository path (default: .)
#   --origin-remote <name>      Push remote (default: origin)
#   --upstream-remote <name>    Sync remote (default: upstream)
#   --onto <ref>                Override sync target (default: upstream/<default-branch>)
#   --no-verify                 Pass --no-verify to git push
#   --verbose                   Show skip reasons per repo
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
REPO_EXPLICIT=0
VERBOSE=0

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
Usage: smart-sync-upstream-force-push.sh --provider <name> --model <name> [options]

Sync with upstream then push --force-with-lease to origin.

Required:
  --provider <name>           AI provider (opencode, codex, copilot)
  --model <name>              AI model name

Options:
  --repo <path>              Target repository path (default: .)
  --origin-remote <name>      Push remote (default: origin)
  --upstream-remote <name>    Sync remote (default: upstream)
  --onto <ref>                Override sync target (default: upstream/<default-branch>)
  --no-verify                 Pass --no-verify to git push
  --verbose                   Show skip reasons per repo
  --dry-run                   Show what would be done
  -h, --help                  Show help
EOF
}

passthrough_args=()

resolve_workspace_root() {
  if [[ -n "${KANO_GIT_MASTER_ROOT:-}" ]]; then
    (cd "$KANO_GIT_MASTER_ROOT" && pwd)
    return 0
  fi
  git rev-parse --show-toplevel 2>/dev/null || true
}

discover_workspace_repos() {
  local root="$1"
  local scripts_root=""
  local repos_json=""
  local path=""

  # SCRIPT_DIR: <repo>/scripts/commit-tools/sync
  # scripts_root should be <repo>/scripts
  scripts_root="$(cd "$SCRIPT_DIR/../.." && pwd)"
  repos_json="$("$scripts_root/core/discover-repos.sh" --root "$root" --format json --include-types root,submodule,standalone 2>/dev/null || true)"
  echo "$repos_json" | grep -o '{[^}]*}' | while IFS= read -r repo; do
    [[ -z "$repo" ]] && continue
    path="$(printf '%s' "$repo" | sed -n 's/.*"path":"\([^"]*\)".*/\1/p')"
    [[ -z "$path" ]] && continue
    printf '%s\n' "$path"
  done
}

sync_force_push_one_repo() {
  local repo_path="$1"

  if ! validate_repo "$repo_path"; then
    [[ "$VERBOSE" -eq 1 ]] && echo "[$repo_path] SKIP: not a git repo"
    return 0
  fi

  if ! is_clean_working_tree "$repo_path"; then
    [[ "$VERBOSE" -eq 1 ]] && echo "[$repo_path] SKIP: dirty working tree"
    return 0
  fi

  if ! git -C "$repo_path" remote get-url "$ORIGIN_REMOTE" >/dev/null 2>&1; then
    [[ "$VERBOSE" -eq 1 ]] && echo "[$repo_path] SKIP: missing origin remote ($ORIGIN_REMOTE)"
    return 0
  fi

  if ! git -C "$repo_path" remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
    [[ "$VERBOSE" -eq 1 ]] && echo "[$repo_path] SKIP: missing upstream remote ($UPSTREAM_REMOTE)"
    return 0
  fi

  current_branch="$(get_current_branch "$repo_path")"
  if [[ -z "$current_branch" ]]; then
    [[ "$VERBOSE" -eq 1 ]] && echo "[$repo_path] SKIP: detached HEAD"
    return 0
  fi

  git -C "$repo_path" fetch "$UPSTREAM_REMOTE" --prune >/dev/null 2>&1 || true
  git -C "$repo_path" fetch "$ORIGIN_REMOTE" --prune >/dev/null 2>&1 || true

  effective_onto="$ONTO_REF"
  if [[ -z "$effective_onto" ]]; then
    upstream_default="$(detect_remote_default_branch "$repo_path" "$UPSTREAM_REMOTE" || true)"
    if [[ -z "${upstream_default:-}" ]]; then
      echo "[$repo_path] ERROR: Could not detect default branch for remote: $UPSTREAM_REMOTE" >&2
      return 1
    fi
    effective_onto="$UPSTREAM_REMOTE/$upstream_default"
  fi

  sync_cmd=("$SCRIPT_DIR/smart-sync.sh" --provider "$AI_PROVIDER" --model "$AI_MODEL" --onto "$effective_onto")
  if [[ "$DRY_RUN" -eq 1 ]]; then
    sync_cmd+=("--dry-run")
  fi
  sync_cmd+=("${passthrough_args[@]}")

  push_cmd=(git -C "$repo_path" push --force-with-lease "$ORIGIN_REMOTE" "$current_branch")
  if [[ "$NO_VERIFY" -eq 1 ]]; then
    push_cmd=(git -C "$repo_path" push --no-verify --force-with-lease "$ORIGIN_REMOTE" "$current_branch")
  fi

  echo "=== Upstream Sync + Force Push: $repo_path ==="
  echo "Syncing $current_branch → $effective_onto"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[DRY RUN] Would run: ${sync_cmd[*]}"
    echo "[DRY RUN] Would run: ${push_cmd[*]}"
    return 0
  fi

  (
    cd "$repo_path"
    bash "${sync_cmd[@]}"
  )
  echo "Force-pushing to $ORIGIN_REMOTE/$current_branch (with --force-with-lease)"
  "${push_cmd[@]}"
  return 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo)
      REPO="${2:-}"
      REPO_EXPLICIT=1
      shift 2
      ;;
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
    --verbose)
      VERBOSE=1
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

if [[ "$REPO_EXPLICIT" -eq 1 ]]; then
  # Explicit single-repo mode.
  sync_force_push_one_repo "$REPO"
  exit $?
fi

# Auto mode:
# If current repo has upstream configured, operate on it; otherwise operate on all repos in the workspace
# that have an upstream remote configured.
if validate_repo "$REPO" >/dev/null 2>&1 && git -C "$REPO" remote get-url "$UPSTREAM_REMOTE" >/dev/null 2>&1; then
  sync_force_push_one_repo "$REPO"
  exit $?
fi

workspace_root="$(resolve_workspace_root)"
if [[ -z "${workspace_root:-}" ]]; then
  echo "ERROR: Could not resolve workspace root. Set KANO_GIT_MASTER_ROOT or run inside a git repo." >&2
  exit 1
fi

echo "INFO: No upstream remote in current repo; scanning workspace for repos with '$UPSTREAM_REMOTE'..."
failed=0
while IFS= read -r repo_path; do
  [[ -z "$repo_path" ]] && continue
  if ! sync_force_push_one_repo "$repo_path"; then
    failed=1
  fi
done < <(discover_workspace_repos "$workspace_root")

exit "$failed"
