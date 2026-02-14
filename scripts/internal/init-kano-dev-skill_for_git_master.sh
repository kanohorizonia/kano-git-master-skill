#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

LIB_DIR="$(cd "$SCRIPT_DIR/../lib" && pwd)"
if [[ -f "$LIB_DIR/git-helpers.sh" ]]; then
  source "$LIB_DIR/git-helpers.sh"
else
  echo "ERROR: Cannot find git-helpers.sh at $LIB_DIR/git-helpers.sh" >&2
  exit 1
fi

ensure_backlog_repo() {
  local backlog_dir="${BACKLOG_DIR:-$SKILL_ROOT/_kano/backlog}"
  local backlog_ssh="${BACKLOG_SSH:-git@github.com:dorgonman/kano-skill-dev-backlog.git}"
  local backlog_https="${BACKLOG_HTTPS:-https://github.com/dorgonman/kano-skill-dev-backlog.git}"

  mkdir -p "$(dirname "$backlog_dir")"

  if [[ ! -d "$backlog_dir/.git" ]]; then
    if [[ -d "$backlog_dir" && -n "$(ls -A "$backlog_dir" 2>/dev/null || true)" ]]; then
      gith_error "Backlog dir exists but is not a git repo: $backlog_dir"
      gith_error "Please remove it or make it a git clone of kano-skill-dev-backlog."
      return 1
    fi

    local clone_url="$backlog_ssh"
    if ! git ls-remote "$clone_url" HEAD >/dev/null 2>&1; then
      clone_url="$backlog_https"
    fi

    gith_log "INFO" "Cloning backlog repo into: $backlog_dir"
    git clone "$clone_url" "$backlog_dir"
  fi

  if ! gith_is_git_repo "$backlog_dir"; then
    gith_error "Backlog dir is not a git repo: $backlog_dir"
    return 1
  fi

  if [[ -n "$(git -C "$backlog_dir" status --porcelain)" ]]; then
    gith_error "Backlog repo has local changes; refusing to auto-update: $backlog_dir"
    gith_error "Please commit/reset changes, then re-run."
    return 1
  fi

  "$SCRIPT_DIR/../core/setup-multi-remote.sh" \
    --origin-ssh "$backlog_ssh" \
    --origin-http "$backlog_https" \
    --dir "$backlog_dir"

  local fetch_remote="origin-ssh"
  if ! git -C "$backlog_dir" remote get-url "$fetch_remote" >/dev/null 2>&1; then
    fetch_remote="origin-http"
  fi
  if ! git -C "$backlog_dir" remote get-url "$fetch_remote" >/dev/null 2>&1; then
    fetch_remote="origin"
  fi

  gith_log "INFO" "Updating backlog repo from $fetch_remote"
  git -C "$backlog_dir" fetch "$fetch_remote"
  git -C "$backlog_dir" checkout main
  if git -C "$backlog_dir" show-ref --verify --quiet "refs/remotes/$fetch_remote/main"; then
    git -C "$backlog_dir" rebase "$fetch_remote/main"
  elif git -C "$backlog_dir" show-ref --verify --quiet "refs/remotes/origin/main"; then
    git -C "$backlog_dir" rebase "origin/main"
  fi
}

REPO_SSH="${REPO_SSH:-git@github.com:dorgonman/kano-agent-skill.git}"
REPO_HTTPS="${REPO_HTTPS:-https://github.com/dorgonman/kano-agent-skill.git}"
REPO_DIR="${REPO_DIR:-$SKILL_ROOT/skills/kano}"
TOOLING_BRANCH="${TOOLING_BRANCH:-dev/kano-git-master-tooling}"

SKILL_1_SSH="${SKILL_1_SSH:-git@github.com:dorgonman/kano-filesystem-safe-ops-skill.git}"
SKILL_1_HTTPS="${SKILL_1_HTTPS:-https://github.com/dorgonman/kano-filesystem-safe-ops-skill.git}"
SKILL_1_PATH="${SKILL_1_PATH:-kano-filesystem-safe-ops-skill}"

SKILL_2_SSH="${SKILL_2_SSH:-git@github.com:dorgonman/kano-agent-backlog-skill.git}"
SKILL_2_HTTPS="${SKILL_2_HTTPS:-https://github.com/dorgonman/kano-agent-backlog-skill.git}"
SKILL_2_PATH="${SKILL_2_PATH:-kano-agent-backlog-skill}"

ensure_backlog_repo

"$SCRIPT_DIR/init-kano-dev-skill.sh" \
  --repo-ssh "$REPO_SSH" \
  --repo-https "$REPO_HTTPS" \
  --repo-dir "$REPO_DIR" \
  --tooling-branch "$TOOLING_BRANCH" \
  --update-tooling \
  --skill "$SKILL_1_SSH|$SKILL_1_HTTPS|$SKILL_1_PATH" \
  --skill "$SKILL_2_SSH|$SKILL_2_HTTPS|$SKILL_2_PATH"
