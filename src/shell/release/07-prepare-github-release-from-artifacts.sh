#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="${1:-$(pwd)}"
ARTIFACT_ROOT="${2:-artifacts}"
shift $(( $# >= 1 ? 1 : 0 ))
shift $(( $# >= 1 ? 1 : 0 ))

cd "$REPO_ROOT"

detect_host_platform() {
  if [ -n "${KANO_RELEASE_PREPARE_HOST_PLATFORM:-}" ]; then
    printf '%s\n' "$KANO_RELEASE_PREPARE_HOST_PLATFORM"
    return 0
  fi
  case "$(uname -s 2>/dev/null || echo unknown)" in
    Darwin*) printf '%s\n' "macos" ;;
    Linux*) printf '%s\n' "linux" ;;
    MINGW*|MSYS*|CYGWIN*) printf '%s\n' "windows" ;;
    *) printf '%s\n' "unknown" ;;
  esac
}

HOST_PLATFORM="$(detect_host_platform)"

host_binary_names() {
  case "$HOST_PLATFORM" in
    windows) printf '%s\n' "kano-git.exe" "kano-git" ;;
    *) printf '%s\n' "kano-git" ;;
  esac
}

host_binary_patterns() {
  local name
  for name in $(host_binary_names); do
    printf '%s\n' "*/out/bin/$name"
    printf '%s\n' "*/out/bin/*/release/$name"
    printf '%s\n' "*/bin/*/release/$name"
  done
}

host_archive_patterns() {
  case "$HOST_PLATFORM" in
    macos)
      printf '%s\n' "*macos-x64*Release-cli*.tar*" "*macos*Release-cli*.tar*" "*mac*Release-cli*.tar*" "*macos*cli*.tar*"
      ;;
    linux)
      printf '%s\n' "*linux-x64*Release-cli*.tar*" "*linux*Release-cli*.tar*" "*linux*cli*.tar*"
      ;;
    windows)
      printf '%s\n' "*windows-x64*Release-cli*.tar*" "*windows*Release-cli*.tar*" "*windows*cli*.tar*"
      ;;
  esac
  printf '%s\n' "*Release-cli*.tar*" "*cli*.tar*"
}

find_existing_binary() {
  local name candidate pattern
  for name in "${KANO_GIT_BINARY_PATH:-}" "${KANO_GIT_BIN:-}"; do
    [ -n "$name" ] || continue
    if [ -f "$name" ]; then
      printf '%s\n' "$name"
      return 0
    fi
    for candidate_name in $(host_binary_names); do
      if [ -f "$name/$candidate_name" ]; then
        printf '%s\n' "$name/$candidate_name"
        return 0
      fi
    done
  done

  while IFS= read -r pattern; do
    candidate="$(find src/cpp/out/bin src/cpp/build/bin build/bin "$ARTIFACT_ROOT" -type f -path "$pattern" 2>/dev/null | sort | head -n 1 || true)"
    if [ -n "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done < <(host_binary_patterns)
}

find_cli_archive() {
  local pattern found
  while IFS= read -r pattern; do
    found="$(find "$ARTIFACT_ROOT" -maxdepth 1 -type f -name "$pattern" | sort | head -n 1 || true)"
    if [ -n "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
  done < <(host_archive_patterns)
}

extract_binary_from_archive() {
  local archive="$1"
  local extract_root=".kano/tmp/release-github-prepare-tools"
  local binary

  rm -rf "$extract_root"
  mkdir -p "$extract_root"
  tar -xf "$archive" -C "$extract_root"
  while IFS= read -r pattern; do
    binary="$(find "$extract_root" -type f -path "$pattern" | sort | head -n 1 || true)"
    if [ -n "$binary" ]; then
      break
    fi
  done < <(host_binary_patterns)
  if [ -z "$binary" ]; then
    echo "ERROR: no host-compatible kano-git binary found after extracting $archive for host=$HOST_PLATFORM" >&2
    return 1
  fi
  chmod +x "$binary" 2>/dev/null || true
  printf '%s\n' "$binary"
}

binary="$(find_existing_binary || true)"
if [ -z "$binary" ]; then
  archive="$(find_cli_archive || true)"
  if [ -z "$archive" ]; then
    echo "ERROR: no kano-git binary or cli archive found under $ARTIFACT_ROOT" >&2
    exit 121
  fi
  binary="$(extract_binary_from_archive "$archive")"
fi

echo "release-github-prepare: host=$HOST_PLATFORM using kano-git binary: $binary"
"$binary" release github prepare --repo . --dry-run "$@"
