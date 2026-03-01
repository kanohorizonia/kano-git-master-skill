#!/usr/bin/env bash
set -euo pipefail

_kog_trim() {
  local value="$1"
  value="${value#"${value%%[![:space:]]*}"}"
  value="${value%"${value##*[![:space:]]}"}"
  printf '%s' "$value"
}

_kog_default_unknown() {
  local value="$1"
  if [[ -z "$value" ]]; then
    printf '%s' "unknown"
    return
  fi
  printf '%s' "$value"
}

kog_collect_build_metadata() {
  local root="${KOG_CPP_ROOT:-$(pwd)}"
  local vcs="unknown"
  local branch="unknown"
  local revision="unknown"
  local hash_short="unknown"
  local hash_full="unknown"
  local dirty="unknown"
  local timestamp_utc
  local host_name
  local ci
  local pipeline_id
  local platform="${KOG_BUILD_PLATFORM:-$(uname -s 2>/dev/null || printf 'unknown')-$(uname -m 2>/dev/null || printf 'unknown')}"

  timestamp_utc="$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || printf 'unknown')"
  host_name="${KOG_BUILD_HOST_NAME:-${HOSTNAME:-$(hostname 2>/dev/null || printf 'unknown')}}"
  if [[ -n "${CI:-}" ]]; then
    ci="true"
  else
    ci="false"
  fi
  pipeline_id="${KOG_BUILD_PIPELINE_ID:-${GITHUB_RUN_ID:-${CI_PIPELINE_ID:-${BUILD_BUILDID:-${BUILD_NUMBER:-unknown}}}}}"

  if command -v git >/dev/null 2>&1 && (cd "$root" && git rev-parse --is-inside-work-tree >/dev/null 2>&1); then
    vcs="git"
    branch="$( (cd "$root" && git symbolic-ref --short HEAD 2>/dev/null) || true )"
    revision="$( (cd "$root" && git rev-list --count --first-parent HEAD 2>/dev/null) || true )"
    hash_short="$( (cd "$root" && git rev-parse --short HEAD 2>/dev/null) || true )"
    hash_full="$( (cd "$root" && git rev-parse HEAD 2>/dev/null) || true )"
    if [[ -n "$( (cd "$root" && git status --porcelain 2>/dev/null) || true )" ]]; then
      dirty="true"
    else
      dirty="false"
    fi
  elif command -v svn >/dev/null 2>&1 && (cd "$root" && svn info >/dev/null 2>&1); then
    vcs="svn"
    branch="$( (cd "$root" && svn info --show-item relative-url 2>/dev/null) || true )"
    branch="${branch#^/}"
    revision="$( (cd "$root" && svn info --show-item revision 2>/dev/null) || true )"
    if [[ -n "$( (cd "$root" && svn status -q 2>/dev/null) || true )" ]]; then
      dirty="true"
    else
      dirty="false"
    fi
  elif command -v p4 >/dev/null 2>&1; then
    vcs="p4"
    branch="$(p4 switch 2>/dev/null || true)"
    revision="$(p4 changes -m1 ...#have 2>/dev/null | grep -Eo 'Change *[0-9:]+' | grep -Eo '[0-9]{1,9}' | sed -n '1p' || true)"
    dirty="unknown"
  fi

  export KOG_BUILD_VCS="$(_kog_default_unknown "$(_kog_trim "$vcs")")"
  export KOG_BUILD_BRANCH="$(_kog_default_unknown "$(_kog_trim "$branch")")"
  export KOG_BUILD_REVISION="$(_kog_default_unknown "$(_kog_trim "$revision")")"
  export KOG_BUILD_REVISION_HASH_SHORT="$(_kog_default_unknown "$(_kog_trim "$hash_short")")"
  export KOG_BUILD_REVISION_HASH="$(_kog_default_unknown "$(_kog_trim "$hash_full")")"
  export KOG_BUILD_DIRTY="$(_kog_default_unknown "$(_kog_trim "$dirty")")"
  export KOG_BUILD_TIMESTAMP_UTC="$(_kog_default_unknown "$(_kog_trim "$timestamp_utc")")"
  export KOG_BUILD_HOST_NAME="$(_kog_default_unknown "$(_kog_trim "$host_name")")"
  export KOG_BUILD_CI="$(_kog_default_unknown "$(_kog_trim "$ci")")"
  export KOG_BUILD_PIPELINE_ID="$(_kog_default_unknown "$(_kog_trim "$pipeline_id")")"
  export KOG_BUILD_PLATFORM="$(_kog_default_unknown "$(_kog_trim "$platform")")"
}
