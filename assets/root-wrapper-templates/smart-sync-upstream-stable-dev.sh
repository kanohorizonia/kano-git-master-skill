#!/usr/bin/env bash
#
# smart-sync-upstream-stable-dev.sh - Project-level wrapper
#
# Upstream stable dev flow:
#   - Create/switch branch from latest stable upstream tag
#   - Cherry-pick prior fixes from previous stable dev branch
#   - Push to origin

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SKILL_SCRIPT="$ROOT/skills/kano/kano-git-master-skill/scripts/commit-tools/sync/smart-sync-stable-dev.sh"

if [[ ! -f "$SKILL_SCRIPT" ]]; then
  echo "ERROR: Git Master Skill script not found at:" >&2
  echo "  $SKILL_SCRIPT" >&2
  echo "Ensure the kano-git-master-skill submodule is initialized." >&2
  exit 1
fi

export KANO_GIT_MASTER_ROOT="$ROOT"

STABLE_TAG_PATTERN='^(release[-_/])?(v)?[0-9]+(\.[0-9]+){1,3}(\+[0-9A-Za-z.-]+)?$'

resolve_current_branch() {
  local repo="$1"
  git -C "$repo" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "detached"
}

resolve_latest_stable_tag() {
  local repo="$1"
  git -C "$repo" tag --list --sort=-version:refname | grep -Ei "$STABLE_TAG_PATTERN" | head -n 1 || true
}

resolve_commit_sha() {
  local repo="$1"
  local rev="$2"
  git -C "$repo" rev-parse "$rev" 2>/dev/null || true
}

resolve_commit_line() {
  local repo="$1"
  local rev="$2"
  if [[ -z "$rev" ]]; then
    echo "N/A"
    return 0
  fi
  git -C "$repo" show -s --format='%h | %cI | %an | %s' "$rev" 2>/dev/null || echo "N/A"
}

resolve_gitmodules_branch() {
  local rel="$1"
  local section_key
  section_key="$(git config -f "$ROOT/.gitmodules" --get-regexp '^submodule\..*\.path$' | awk -v p="$rel" '$2==p {print $1; exit}')"
  [[ -z "$section_key" ]] && return 1
  section_key="${section_key%.path}"
  git config -f "$ROOT/.gitmodules" --get "$section_key.branch" 2>/dev/null || true
}

resolve_stable_ref() {
  local repo="$1"
  local current_branch="$2"
  local rel="$3"

  if [[ "$current_branch" =~ ^branch_ ]]; then
    echo "$current_branch"
    return 0
  fi

  local gm_branch=""
  gm_branch="$(resolve_gitmodules_branch "$rel" || true)"
  if [[ -n "$gm_branch" ]]; then
    if git -C "$repo" show-ref --verify --quiet "refs/heads/$gm_branch"; then
      echo "$gm_branch"
      return 0
    fi
    if git -C "$repo" show-ref --verify --quiet "refs/remotes/origin/$gm_branch"; then
      echo "origin/$gm_branch"
      return 0
    fi
  fi

  echo "$current_branch"
}

append_repo_summary() {
  local summary_file="$1"
  local rel="$2"
  local status="$3"
  local reason="$4"
  local repo="$ROOT/$rel"
  local branch upstream_sha stable_sha upstream_line stable_line latest_tag stable_ref

  if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$rel" "$status" "N/A" "0" "N/A" "N/A" >>"$summary_file"
    return 0
  fi

  branch="$(resolve_current_branch "$repo")"
  latest_tag=""
  upstream_sha=""
  if git -C "$repo" remote | grep -q '^upstream$'; then
    latest_tag="$(resolve_latest_stable_tag "$repo")"
    if [[ -n "$latest_tag" ]]; then
      upstream_sha="$(resolve_commit_sha "$repo" "refs/tags/$latest_tag^{commit}")"
    fi
  fi

  stable_ref="$(resolve_stable_ref "$repo" "$branch" "$rel")"
  stable_sha="$(resolve_commit_sha "$repo" "$stable_ref")"
  upstream_line="$(resolve_commit_line "$repo" "$upstream_sha")"
  stable_line="$(resolve_commit_line "$repo" "$stable_sha")"

  if [[ -n "$upstream_sha" && -n "$stable_sha" && "$upstream_sha" == "$stable_sha" ]]; then
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$rel" "$status" "$branch" "1" "$upstream_line" "(same as upstream)" >>"$summary_file"
  else
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$rel" "$status" "$branch" "0" "$upstream_line" "$stable_line" >>"$summary_file"
  fi
}

truncate_text() {
  local text="$1"
  local max="${2:-80}"
  if (( ${#text} <= max )); then
    printf '%s' "$text"
  else
    printf '%s...' "${text:0:max-3}"
  fi
}

print_summary_table() {
  local summary_file="$1"
  local sep="+------------------------------+------------+------------------------+----------------------------------------------------------------------------------+----------------------------------------------------------------------------------+"
  printf '%s\n' "$sep"
  printf '| %-28s | %-10s | %-22s | %-80s | %-80s |\n' \
    "Repo" "Status" "Current Branch" "Latest Upstream Commit (sha|time|author|title)" "Latest Stable Commit (sha|time|author|title)"
  printf '%s\n' "$sep"

  while IFS=$'\t' read -r rel status branch same_flag upstream_line stable_line; do
    local up st
    up="$(truncate_text "$upstream_line" 80)"
    st="$(truncate_text "$stable_line" 80)"
    printf '| %-28s | %-10s | %-22s | %-80s | %-80s |\n' \
      "$(truncate_text "$rel" 28)" \
      "$(truncate_text "$status" 10)" \
      "$(truncate_text "$branch" 22)" \
      "$up" \
      "$st"
  done <"$summary_file"
  printf '%s\n' "$sep"
}

print_summary_compact() {
  local summary_file="$1"
  while IFS=$'\t' read -r rel status branch same_flag upstream_line stable_line; do
    echo "[$rel] $status | branch=$branch"
    if [[ "$same_flag" == "1" ]]; then
      echo "  commit: $upstream_line"
    else
      echo "  upstream: $upstream_line"
      echo "  stable:   $stable_line"
    fi
  done <"$summary_file"
}

print_summary_tsv() {
  local summary_file="$1"
  echo -e "repo\tstatus\tcurrent_branch\tsame_commit\tlatest_upstream_commit\tlatest_stable_commit"
  cat "$summary_file"
}

json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  s="${s//$'\n'/\\n}"
  s="${s//$'\r'/\\r}"
  s="${s//$'\t'/\\t}"
  printf '%s' "$s"
}

print_summary_json() {
  local summary_file="$1"
  local first=1
  echo "["
  while IFS=$'\t' read -r rel status branch same_flag upstream_line stable_line; do
    [[ "$first" -eq 0 ]] && echo ","
    first=0
    printf '  {"repo":"%s","status":"%s","current_branch":"%s","same_commit":%s,"latest_upstream_commit":"%s","latest_stable_commit":"%s"}' \
      "$(json_escape "$rel")" \
      "$(json_escape "$status")" \
      "$(json_escape "$branch")" \
      "$([[ "$same_flag" == "1" ]] && echo "true" || echo "false")" \
      "$(json_escape "$upstream_line")" \
      "$(json_escape "$stable_line")"
  done <"$summary_file"
  echo
  echo "]"
}

print_summary_markdown() {
  local summary_file="$1"
  echo "| Repo | Status | Current Branch | Latest Upstream Commit | Latest Stable Commit |"
  echo "| --- | --- | --- | --- | --- |"
  while IFS=$'\t' read -r rel status branch same_flag upstream_line stable_line; do
    local up st
    up="${upstream_line//|/\\|}"
    st="${stable_line//|/\\|}"
    echo "| $rel | $status | $branch | $up | $st |"
  done <"$summary_file"
}

REPORT_FORMAT="compact"
has_repo_arg=0
forward_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --format)
      REPORT_FORMAT="${2:-}"
      shift 2
      ;;
    --format=*)
      REPORT_FORMAT="${1#--format=}"
      shift
      ;;
    --repo)
      has_repo_arg=1
      forward_args+=("$1" "${2:-}")
      shift 2
      ;;
    *)
      forward_args+=("$1")
      shift
      ;;
  esac
done

if [[ "$has_repo_arg" -eq 1 ]]; then
  exec bash "$SKILL_SCRIPT" "${forward_args[@]}"
fi

if [[ ! -f "$ROOT/.gitmodules" ]]; then
  echo "ERROR: .gitmodules not found at project root: $ROOT/.gitmodules" >&2
  exit 1
fi

mapfile -t submodule_paths < <(git config -f "$ROOT/.gitmodules" --get-regexp '^submodule\..*\.path$' | awk '{print $2}' | grep -E '^src/' || true)
if [[ ${#submodule_paths[@]} -eq 0 ]]; then
  echo "INFO: No src/* submodules found. Nothing to do."
  exit 0
fi

case "$REPORT_FORMAT" in
  compact|table|tsv|json|markdown) ;;
  *)
    echo "ERROR: Unsupported --format: $REPORT_FORMAT (supported: compact, table, tsv, json, markdown)" >&2
    exit 1
    ;;
esac

passed_args=("${forward_args[@]}")
processed=0
skipped=0
failed=0
summary_file="$(mktemp)"
trap 'rm -f "$summary_file"' EXIT

for rel in "${submodule_paths[@]}"; do
  repo="$ROOT/$rel"
  if ! git -C "$repo" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "SKIP: $rel (not initialized git repo)"
    skipped=$((skipped + 1))
    append_repo_summary "$summary_file" "$rel" "skipped" "not initialized git repo"
    continue
  fi
  if ! git -C "$repo" remote | grep -q '^upstream$'; then
    echo "SKIP: $rel (no upstream remote)"
    skipped=$((skipped + 1))
    append_repo_summary "$summary_file" "$rel" "skipped" "no upstream remote"
    continue
  fi

  echo "RUN: $rel"
  if bash "$SKILL_SCRIPT" --repo "$repo" "${passed_args[@]}"; then
    processed=$((processed + 1))
    append_repo_summary "$summary_file" "$rel" "processed" ""
  else
    echo "FAIL: $rel" >&2
    failed=$((failed + 1))
    append_repo_summary "$summary_file" "$rel" "failed" "workflow failed"
  fi
done

echo "=== upstream-stable-dev wrapper summary ==="
echo "processed: $processed"
echo "skipped: $skipped"
echo "failed: $failed"
echo "=== upstream-stable-dev branch report ==="
case "$REPORT_FORMAT" in
  compact) print_summary_compact "$summary_file" ;;
  table) print_summary_table "$summary_file" ;;
  tsv) print_summary_tsv "$summary_file" ;;
  json) print_summary_json "$summary_file" ;;
  markdown) print_summary_markdown "$summary_file" ;;
esac

if [[ "$failed" -gt 0 ]]; then
  exit 1
fi
