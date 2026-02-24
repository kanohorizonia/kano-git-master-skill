#!/usr/bin/env bash
#
# submodule-common.sh - shared helpers for submodule scripts

subm_require_git_repo() {
  if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "Error: Not in a git repository" >&2
    return 1
  fi
}

subm_require_gitmodules() {
  if [[ ! -f .gitmodules ]]; then
    echo "Error: No .gitmodules file found" >&2
    return 1
  fi
}

subm_has_submodules() {
  git config --file .gitmodules --get-regexp path >/dev/null 2>&1
}

subm_print_submodule_status() {
  git submodule status | while read -r line; do
    [[ -n "$line" ]] && echo "  $line"
  done
}

subm_run() {
  local dry_run="${1:-false}"
  shift || true
  if [[ "$dry_run" == "true" ]]; then
    printf '+ %q' "$@"
    printf '\n'
    return 0
  fi
  "$@"
}

