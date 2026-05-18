#!/usr/bin/env bash
#
# audit-git-index-hygiene.sh
# Shell-only guard for the Git-index hygiene rules that must be true before a
# commit/export. This intentionally does not require the native kog binary.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$repo_root"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "SKIP: Git index hygiene audit requires a Git worktree."
  exit 0
fi

failures=0

is_required_executable() {
  local path="$1"
  case "$path" in
    scripts/kog|scripts/kano-git|scripts/setup-global-tools.sh)
      return 0
      ;;
    assets/root-wrapper-templates/common/kog)
      return 0
      ;;
    assets/root-wrapper-templates/common/kog-*.sh)
      return 0
      ;;
    assets/root-wrapper-templates/profiles/*/*.sh)
      return 0
      ;;
    src/shell/*.sh|src/shell/*/*.sh|src/shell/*/*/*.sh)
      return 0
      ;;
    setup_benchmark_env.sh)
      return 0
      ;;
    .githooks/pre-commit|.githooks/pre-push)
      return 0
      ;;
  esac
  return 1
}

is_required_lf() {
  local path="$1"
  case "$path" in
    .gitattributes|.gitignore|.gitmodules|.editorconfig|.kogignore|VERSION)
      return 0
      ;;
    *.sh|*.bash|*.zsh|*.py|*.cpp|*.hpp|*.h|*.c|*.cppm|*.cmake|CMakeLists.txt)
      return 0
      ;;
    *.groovy|*.md|*.toml|*.yml|*.yaml|*.json|*.ini|*.cs|*.uproject|*.rules|*.txt)
      return 0
      ;;
    scripts/kog|scripts/kano-git|assets/root-wrapper-templates/common/kog)
      return 0
      ;;
    .githooks/*)
      return 0
      ;;
  esac
  return 1
}

while IFS=$'\t' read -r meta path; do
  [[ -n "${path:-}" ]] || continue
  mode="${meta%% *}"
  if is_required_executable "$path" && [[ "$mode" != "100755" ]]; then
    echo "Error: executable bit missing for $path (tracked as $mode, expected 100755)." >&2
    failures=$((failures + 1))
  fi
done < <(git ls-files -s)

while IFS=$'\t' read -r eol_info path; do
  [[ -n "${path:-}" ]] || continue
  if [[ "$eol_info" == *"attr/-text"* || "$eol_info" == *"attr/binary"* ]]; then
    continue
  fi
  if is_required_lf "$path"; then
    if [[ "$eol_info" == *"i/crlf"* ]]; then
      echo "Error: CRLF detected in Git index for LF-required file: $path" >&2
      failures=$((failures + 1))
    fi
    if [[ "$eol_info" != *"eol=lf"* ]]; then
      echo "Warning: missing eol=lf attribute for LF-required file: $path" >&2
    fi
  fi
done < <(git ls-files --eol)

if (( failures > 0 )); then
  echo "FAIL: Git index hygiene audit found $failures issue(s)." >&2
  exit 1
fi

echo "PASS: Git index hygiene audit"
