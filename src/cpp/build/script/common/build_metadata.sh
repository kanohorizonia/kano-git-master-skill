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

kog_workspace_root() {
  local cpp_root="${KOG_CPP_ROOT:-$(pwd)}"
  (cd "$cpp_root/../.." && pwd)
}

_kog_extract_toml_section_value() {
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
  local key_name="$1"
  local workspace_root
  local home_dir="${HOME:-}"
  local value=""
  local file_path=""
  workspace_root="$(kog_workspace_root)"

  for file_path in \
    "$workspace_root/assets/kog_config.toml" \
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
  if [[ -z "${KOG_COMPILER_LAUNCHER:-}" ]]; then
    local configured_launcher=""
    configured_launcher="$(kog_resolve_self_config_value "compiler_launcher")"
    if [[ -n "$configured_launcher" ]]; then
      export KOG_COMPILER_LAUNCHER="$configured_launcher"
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
  local pipeline_id
  local platform="${KOG_BUILD_PLATFORM:-$(uname -s 2>/dev/null || printf 'unknown')-$(uname -m 2>/dev/null || printf 'unknown')}"

  timestamp_utc=""
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
  export KOG_BUILD_PIPELINE_ID="$(_kog_default_unknown "$(_kog_trim "$pipeline_id")")"
  export KOG_BUILD_PLATFORM="$(_kog_default_unknown "$(_kog_trim "$platform")")"
}

kog_ensure_ftxui_vendor() {
  local root="${KOG_CPP_ROOT:-$(pwd)}"
  local thirdparty_dir="$root/code/thirdparty"
  local ftxui_dir="$thirdparty_dir/ftxui"
  local ftxui_repo="${KOG_FTXUI_REPO_URL:-https://github.com/ArthurSonzogni/FTXUI.git}"

  if [[ -f "$ftxui_dir/CMakeLists.txt" ]]; then
    return 0
  fi

  if [[ -d "$ftxui_dir" ]]; then
    if [[ -z "$(find "$ftxui_dir" -mindepth 1 -print -quit 2>/dev/null || true)" ]]; then
      echo "Found empty vendored FTXUI directory. Recreating: $ftxui_dir" >&2
      rmdir "$ftxui_dir" 2>/dev/null || rm -rf "$ftxui_dir"
    else
    echo "Found incomplete vendored FTXUI directory: $ftxui_dir" >&2
    echo "Remove it and rerun, or ensure CMakeLists.txt exists in that directory." >&2
    exit 1
    fi
  fi

  if ! command -v git >/dev/null 2>&1; then
    echo "git is required to fetch missing thirdparty/ftxui." >&2
    exit 1
  fi

  mkdir -p "$thirdparty_dir"
  echo "Vendored FTXUI not found. Cloning from $ftxui_repo ..."
  git clone --depth 1 "$ftxui_repo" "$ftxui_dir"
}
