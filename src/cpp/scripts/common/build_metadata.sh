#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOG_SHARED_BUILD_METADATA_SH="${KOG_CPP_ROOT:-$SCRIPT_DIR/../../shared/infra}/shared/infra/scripts/common/build_metadata.sh"
if [[ ! -f "$KOG_SHARED_BUILD_METADATA_SH" ]]; then
  KOG_SHARED_BUILD_METADATA_SH="$SCRIPT_DIR/../../shared/infra/scripts/common/build_metadata.sh"
fi
if [[ -f "$KOG_SHARED_BUILD_METADATA_SH" ]]; then
  source "$KOG_SHARED_BUILD_METADATA_SH"
fi

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

_kog_lower() {
  printf '%s' "$1" | tr '[:upper:]' '[:lower:]'
}

_kog_home_dir() {
  if [[ -n "${HOME:-}" ]]; then
    printf '%s' "$HOME"
    return 0
  fi
  if [[ -n "${USERPROFILE:-}" ]]; then
    printf '%s' "$USERPROFILE"
    return 0
  fi
  return 1
}

_kog_default_cache_dir_for_launcher() {
  local launcher_name="$1"
  local home_dir=""
  home_dir="$(_kog_home_dir || true)"
  if [[ -z "$home_dir" ]]; then
    return 1
  fi

  case "$launcher_name" in
    sccache)
      printf '%s' "$home_dir/.kano/cache/sccache"
      return 0
      ;;
    ccache)
      printf '%s' "$home_dir/.kano/cache/ccache"
      return 0
      ;;
    fastbuild)
      printf '%s' "$home_dir/.kano/cache/fastbuild"
      return 0
      ;;
  esac
  return 1
}

kog_apply_fastbuild_env() {
  local home_dir=""
  home_dir="$(_kog_home_dir || true)"

  local fastbuild_root="${KOG_FASTBUILD_ROOT:-}"
  if [[ -z "$fastbuild_root" && -d "D:/Application/FASTBuild" ]]; then
    fastbuild_root="D:/Application/FASTBuild"
  fi
  if [[ -n "$fastbuild_root" ]]; then
    export KOG_FASTBUILD_ROOT="$fastbuild_root"
    if [[ -x "$fastbuild_root/FBuild.exe" ]]; then
      export KOG_FASTBUILD_EXECUTABLE="$fastbuild_root/FBuild.exe"
    fi
  fi

  export KOG_COMPILER_LAUNCHER="none"
  unset KOG_COMPILER_LAUNCHER_RESOLVED || true

  local cache_dir="${FASTBUILD_CACHE_PATH:-}"
  if [[ -z "$cache_dir" ]]; then
    cache_dir="$(_kog_default_cache_dir_for_launcher fastbuild || true)"
  fi
  if [[ -n "$cache_dir" ]]; then
    export FASTBUILD_CACHE_PATH="$cache_dir"
    mkdir -p "$cache_dir" >/dev/null 2>&1 || true
  fi

  if [[ -z "${FASTBUILD_BROKERAGE_PATH:-}" ]]; then
    export FASTBUILD_BROKERAGE_PATH='\\nas\workspace\cache\fastbuild\brokerage'
  fi

  if [[ -z "${FASTBUILD_CACHE_MODE:-}" ]]; then
    export FASTBUILD_CACHE_MODE="rw"
  fi

  if [[ -z "${FASTBUILD_TEMP_PATH:-}" && -n "$home_dir" ]]; then
    export FASTBUILD_TEMP_PATH="$home_dir/.kano/cache/fastbuild/tmp"
    mkdir -p "$FASTBUILD_TEMP_PATH" >/dev/null 2>&1 || true
  fi

  echo "[launcher][fastbuild][info] exe=${KOG_FASTBUILD_EXECUTABLE:-unknown} cache_dir=${FASTBUILD_CACHE_PATH:-unknown} brokerage=${FASTBUILD_BROKERAGE_PATH:-unknown} cache_mode=${FASTBUILD_CACHE_MODE:-unknown}" >&2
}

_kog_select_compiler_launcher() {
  local configured="$1"
  local normalized=""
  normalized="$(_kog_lower "$(_kog_trim "$configured")")"

  case "$normalized" in
    ""|none)
      return 1
      ;;
    auto)
      if [[ "$(uname -s 2>/dev/null || true)" == MINGW* || "$(uname -s 2>/dev/null || true)" == MSYS* || "$(uname -s 2>/dev/null || true)" == CYGWIN* ]]; then
        for candidate in ccache ccache.exe sccache sccache.exe; do
          if command -v "$candidate" >/dev/null 2>&1; then
            printf '%s' "$candidate"
            return 0
          fi
        done
      else
        for candidate in ccache ccache.exe sccache sccache.exe; do
          if command -v "$candidate" >/dev/null 2>&1; then
            printf '%s' "$candidate"
            return 0
          fi
        done
      fi
      return 1
      ;;
    sccache)
      for candidate in sccache sccache.exe; do
        if command -v "$candidate" >/dev/null 2>&1; then
          printf '%s' "$candidate"
          return 0
        fi
      done
      return 1
      ;;
    ccache)
      for candidate in ccache ccache.exe; do
        if command -v "$candidate" >/dev/null 2>&1; then
          printf '%s' "$candidate"
          return 0
        fi
      done
      return 1
      ;;
    *)
      if command -v "$configured" >/dev/null 2>&1; then
        printf '%s' "$configured"
        return 0
      fi
      return 1
      ;;
  esac
}

kog_workspace_root() {
  if declare -F kano_cpp_workspace_root >/dev/null 2>&1; then
    kano_cpp_workspace_root
    return 0
  fi
  local cpp_root="${KOG_CPP_ROOT:-$(pwd)}"
  (cd "$cpp_root/../.." && pwd)
}

_kog_extract_toml_section_value() {
  if declare -F _kano_cpp_extract_toml_section_value >/dev/null 2>&1; then
    _kano_cpp_extract_toml_section_value "$@"
    return 0
  fi
  local file_path="$1"
  local section_name="$2"
  local key_name="$3"
  awk -v section="$section_name" -v key="$key_name" '
    function trim(s) {
      sub(/^[[:space:]]+/, "", s)
      sub(/[[:space:]]+$/, "", s)
      return s
    }
    /^[[:space:]]*#/ { next }
    /^[[:space:]]*$/ { next }
    /^[[:space:]]*\[/ {
      current = $0
      sub(/^[[:space:]]*\[/, "", current)
      sub(/\][[:space:]]*$/, "", current)
      current = trim(current)
      in_section = (current == section)
      next
    }
    in_section {
      line = $0
      sub(/[[:space:]]+#.*$/, "", line)
      if (line ~ "^[[:space:]]*" key "[[:space:]]*=") {
        sub("^[[:space:]]*" key "[[:space:]]*=[[:space:]]*", "", line)
        line = trim(line)
        if (line ~ /^".*"$/) {
          sub(/^"/, "", line)
          sub(/"$/, "", line)
        }
        print line
      }
    }
  ' "$file_path" | tail -n 1
}

kog_resolve_self_config_value() {
  if declare -F kano_cpp_resolve_self_config_value >/dev/null 2>&1; then
    kano_cpp_resolve_self_config_value "$1"
    return 0
  fi
  local key_name="$1"
  local workspace_root
  local home_dir="${HOME:-}"
  local value=""
  local file_path=""
  workspace_root="$(kog_workspace_root)"

  for file_path in \
    "$workspace_root/.kano/kog_config.toml" \
    "$home_dir/.kano/kog_config.toml" \
    "$workspace_root/.kano/kog_config.toml"; do
    if [[ -f "$file_path" ]]; then
      local candidate=""
      candidate="$(_kog_extract_toml_section_value "$file_path" "self" "$key_name")"
      if [[ -n "$candidate" ]]; then
        value="$candidate"
      fi
    fi
  done

  printf '%s' "$value"
}

kog_apply_self_build_config() {
  unset KOG_COMPILER_LAUNCHER_RESOLVED || true
  if [[ -z "${KOG_COMPILER_LAUNCHER:-}" ]]; then
    local configured_launcher=""
    configured_launcher="$(kog_resolve_self_config_value "compiler_launcher")"
    if [[ -n "$configured_launcher" ]]; then
      export KOG_COMPILER_LAUNCHER="$configured_launcher"
    fi
  fi

  local resolved_launcher=""
  if resolved_launcher="$(_kog_select_compiler_launcher "${KOG_COMPILER_LAUNCHER:-}")"; then
    local launcher_name
    launcher_name="$(_kog_lower "$(basename "$resolved_launcher" .exe)")"
    export KOG_COMPILER_LAUNCHER_RESOLVED="$resolved_launcher"

    local cache_dir=""
    cache_dir="$(_kog_default_cache_dir_for_launcher "$launcher_name" || true)"
    if [[ "$launcher_name" == "sccache" ]]; then
      if [[ -z "${SCCACHE_DIR:-}" && -n "$cache_dir" ]]; then
        export SCCACHE_DIR="$cache_dir"
      fi
      mkdir -p "${SCCACHE_DIR:-$cache_dir}" >/dev/null 2>&1 || true
      echo "[launcher][compiler-cache][info] launcher=$resolved_launcher cache_dir=${SCCACHE_DIR:-unknown}" >&2
    elif [[ "$launcher_name" == "ccache" ]]; then
      if [[ -z "${CCACHE_DIR:-}" && -n "$cache_dir" ]]; then
        export CCACHE_DIR="$cache_dir"
      fi
      mkdir -p "${CCACHE_DIR:-$cache_dir}" >/dev/null 2>&1 || true
      echo "[launcher][compiler-cache][info] launcher=$resolved_launcher cache_dir=${CCACHE_DIR:-unknown}" >&2
    else
      echo "[launcher][compiler-cache][info] launcher=$resolved_launcher" >&2
    fi
  else
    if [[ -n "${KOG_COMPILER_LAUNCHER:-}" && "$(_kog_lower "${KOG_COMPILER_LAUNCHER}")" != "none" ]]; then
      echo "[launcher][compiler-cache][warn] requested launcher unavailable: ${KOG_COMPILER_LAUNCHER}" >&2
    fi
  fi
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
  local context
  local pipeline_id
  local platform="${KOG_BUILD_PLATFORM:-$(uname -s 2>/dev/null || printf 'unknown')-$(uname -m 2>/dev/null || printf 'unknown')}"

  timestamp_utc=""
  host_name="${KOG_BUILD_HOST_NAME:-${HOSTNAME:-$(hostname 2>/dev/null || printf 'unknown')}}"
  if [[ -n "${CI:-}" ]]; then
    ci="true"
    context="ci"
  else
    ci="false"
    context="local-manual"
  fi
  pipeline_id="${KOG_BUILD_PIPELINE_ID:-${GITHUB_RUN_ID:-${CI_PIPELINE_ID:-${BUILD_BUILDID:-${BUILD_NUMBER:-$context}}}}}"

  if command -v git >/dev/null 2>&1 && (cd "$root" && git rev-parse --is-inside-work-tree >/dev/null 2>&1); then
    vcs="git"
    branch="$( (cd "$root" && git symbolic-ref --short HEAD 2>/dev/null) || true )"
    revision="$( (cd "$root" && git rev-list --count --first-parent HEAD 2>/dev/null) || true )"
    hash_short="$( (cd "$root" && git rev-parse --short HEAD 2>/dev/null) || true )"
    hash_full="$( (cd "$root" && git rev-parse HEAD 2>/dev/null) || true )"
    if [[ -z "$timestamp_utc" ]]; then
      timestamp_utc="$( (cd "$root" && git show -s --format=%cI HEAD 2>/dev/null) || true )"
    fi
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
    if [[ -z "$timestamp_utc" ]]; then
      timestamp_utc="$( (cd "$root" && svn info --show-item last-changed-date 2>/dev/null) || true )"
    fi
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
  export KOG_BUILD_HOST_NAME="$(_kog_default_unknown "$(_kog_trim "$host_name")")"
  export KOG_BUILD_CI="$(_kog_default_unknown "$(_kog_trim "$ci")")"
  export KOG_BUILD_CONTEXT="$(_kog_default_unknown "$(_kog_trim "$context")")"
  export KOG_BUILD_PIPELINE_ID="$(_kog_default_unknown "$(_kog_trim "$pipeline_id")")"
  export KOG_BUILD_PLATFORM="$(_kog_default_unknown "$(_kog_trim "$platform")")"
}
