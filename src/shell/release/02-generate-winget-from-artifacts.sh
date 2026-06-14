#!/bin/bash
set -euo pipefail

REPO_ROOT="${1:-$(pwd)}"
ARTIFACT_ROOT="${2:-artifacts}"
VERSION_TEXT="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")"
TAG_NAME="${KANO_RELEASE_TAG:-v${VERSION_TEXT}}"
REPO_SLUG="${KANO_GITHUB_REPOSITORY:-kanohorizonia/kano-git-master-skill}"
ASSET_BASE_URL="${KANO_RELEASE_ASSET_BASE_URL:-https://github.com/${REPO_SLUG}/releases/download/${TAG_NAME}}"
OUTPUT_ROOT="${KANO_WINGET_OUTPUT_ROOT:-${KANO_PACKAGE_MANAGER_RECIPE_ROOT:-Release/package-managers}/winget}"

find_installer() {
  local root found
  for root in "$ARTIFACT_ROOT" "$ARTIFACT_ROOT/installers" "$ARTIFACT_ROOT/installers/windows" "$REPO_ROOT/src/wix/out"; do
    [ -d "$root" ] || continue
    found="$(find "$root" -maxdepth 1 -type f -iname '*.msi' | sort | head -n 1 || true)"
    if [ -n "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
  done
}

calc_sha256() {
  local path="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$path" | awk '{print $1}'
    return 0
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
    return 0
  fi
  if command -v openssl >/dev/null 2>&1; then
    openssl dgst -sha256 "$path" | awk '{print $NF}'
    return 0
  fi
  echo "ERROR: sha256sum, shasum, or openssl is required to hash $path" >&2
  exit 115
}

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

find_existing_kano_git_binary() {
  local root candidate_name pattern candidate
  for root in "${KANO_GIT_BINARY_PATH:-}" "${KANO_GIT_BIN:-}" ".kano/tmp/release-github-prepare-tools" "$ARTIFACT_ROOT" src/cpp/out/bin src/cpp/build/bin build/bin; do
    [ -n "$root" ] || continue
    [ -e "$root" ] || continue
    if [ -f "$root" ]; then
      printf '%s\n' "$root"
      return 0
    fi
    for candidate_name in $(host_binary_names); do
      candidate="$(find "$root" -type f -name "$candidate_name" 2>/dev/null | sort | head -n 1 || true)"
      if [ -n "$candidate" ]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
  done
  while IFS= read -r pattern; do
    candidate="$(find "$ARTIFACT_ROOT" -type f -path "$pattern" 2>/dev/null | sort | head -n 1 || true)"
    if [ -n "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done < <(host_binary_patterns)
}

find_host_cli_archive() {
  local pattern found
  while IFS= read -r pattern; do
    found="$(find "$ARTIFACT_ROOT" -maxdepth 1 -type f -name "$pattern" | sort | head -n 1 || true)"
    if [ -n "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
  done < <(host_archive_patterns)
}

extract_kano_git_binary_from_archive() {
  local archive="$1"
  local extract_root=".kano/tmp/winget-generate-tools"
  local binary pattern

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

resolve_kano_git_binary() {
  local binary archive
  binary="$(find_existing_kano_git_binary || true)"
  if [ -n "$binary" ]; then
    printf '%s\n' "$binary"
    return 0
  fi
  archive="$(find_host_cli_archive || true)"
  if [ -z "$archive" ]; then
    echo "ERROR: no host-compatible kano-git binary or cli archive found under $ARTIFACT_ROOT for host=$HOST_PLATFORM" >&2
    return 1
  fi
  extract_kano_git_binary_from_archive "$archive"
}

INSTALLER_PATH="$(find_installer)"
if [ -z "$INSTALLER_PATH" ]; then
  echo "ERROR: winget MSI installer not found under $ARTIFACT_ROOT or src/wix/out" >&2
  exit 114
fi

INSTALLER_SHA256="$(calc_sha256 "$INSTALLER_PATH")"
KANO_GIT_BINARY="$(resolve_kano_git_binary)"

echo "winget-generate: host=$HOST_PLATFORM using kano-git binary: $KANO_GIT_BINARY"
"$KANO_GIT_BINARY" release winget generate \
  --repo . \
  --no-dry-run \
  --installer "$INSTALLER_PATH" \
  --installer-sha256 "$INSTALLER_SHA256" \
  --release-asset-base-url "$ASSET_BASE_URL" \
  --output "$OUTPUT_ROOT"
