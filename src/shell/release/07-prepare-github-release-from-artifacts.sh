#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="${1:-$(pwd)}"
ARTIFACT_ROOT="${2:-artifacts}"
shift $(( $# >= 1 ? 1 : 0 ))
shift $(( $# >= 1 ? 1 : 0 ))

cd "$REPO_ROOT"

find_existing_binary() {
  local name candidate pattern
  for name in "${KANO_GIT_BINARY_PATH:-}" "${KANO_GIT_BIN:-}"; do
    [ -n "$name" ] || continue
    if [ -f "$name" ]; then
      printf '%s\n' "$name"
      return 0
    fi
    if [ -f "$name/kano-git.exe" ]; then
      printf '%s\n' "$name/kano-git.exe"
      return 0
    fi
    if [ -f "$name/kano-git" ]; then
      printf '%s\n' "$name/kano-git"
      return 0
    fi
  done

  for pattern in \
    "*/out/bin/kano-git.exe" \
    "*/out/bin/kano-git" \
    "*/out/bin/*/release/kano-git.exe" \
    "*/out/bin/*/release/kano-git" \
    "*/bin/*/release/kano-git.exe" \
    "*/bin/*/release/kano-git"; do
    candidate="$(find src/cpp/out/bin src/cpp/build/bin build/bin "$ARTIFACT_ROOT" -type f -path "$pattern" 2>/dev/null | sort | head -n 1 || true)"
    if [ -n "$candidate" ]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
}

find_cli_archive() {
  local pattern found
  for pattern in \
    "*windows-x64*Release-cli*.tar*" \
    "*windows*Release-cli*.tar*" \
    "*windows*cli*.tar*" \
    "*Release-cli*.tar*" \
    "*cli*.tar*"; do
    found="$(find "$ARTIFACT_ROOT" -maxdepth 1 -type f -name "$pattern" | sort | head -n 1 || true)"
    if [ -n "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
  done
}

extract_binary_from_archive() {
  local archive="$1"
  local extract_root=".kano/tmp/release-github-prepare-tools"
  local binary

  rm -rf "$extract_root"
  mkdir -p "$extract_root"
  tar -xf "$archive" -C "$extract_root"
  binary="$(find "$extract_root" -type f \( -path '*/out/bin/kano-git.exe' -o -path '*/out/bin/kano-git' -o -path '*/out/bin/*/release/kano-git.exe' -o -path '*/out/bin/*/release/kano-git' \) | sort | head -n 1 || true)"
  if [ -z "$binary" ]; then
    echo "ERROR: no kano-git binary found after extracting $archive" >&2
    return 1
  fi
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

echo "release-github-prepare: using kano-git binary: $binary"
"$binary" release github prepare --repo . --dry-run "$@"
