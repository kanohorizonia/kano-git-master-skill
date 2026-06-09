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

INSTALLER_PATH="$(find_installer)"
if [ -z "$INSTALLER_PATH" ]; then
  echo "ERROR: winget MSI installer not found under $ARTIFACT_ROOT or src/wix/out" >&2
  exit 114
fi

INSTALLER_SHA256="$(calc_sha256 "$INSTALLER_PATH")"

./scripts/kog release winget generate \
  --repo . \
  --no-dry-run \
  --installer "$INSTALLER_PATH" \
  --installer-sha256 "$INSTALLER_SHA256" \
  --release-asset-base-url "$ASSET_BASE_URL" \
  --output "$OUTPUT_ROOT"
