#!/bin/bash
set -euo pipefail

if [ $# -ne 0 ] && [ $# -ne 5 ]; then
  echo "Usage: $0 [<repo-root> <tag-name> <repo-slug> <artifact-dir> <output-dir>]" >&2
  exit 1
fi

REPO_ROOT="${1:-$(pwd)}"
VERSION_TEXT="$(tr -d '[:space:]' < "$REPO_ROOT/VERSION")"
TAG_NAME="${2:-${KANO_RELEASE_TAG:-v${VERSION_TEXT}}}"
REPO_SLUG="${3:-${KANO_GITHUB_REPOSITORY:-kanohorizonia/kano-git-master-skill}}"
ARTIFACT_DIR="${4:-${KANO_INSTALLER_OUTPUT_ROOT:-artifacts/installers}}"
OUTPUT_DIR="${5:-${KANO_PACKAGE_MANAGER_RECIPE_ROOT:-Release/package-managers}/winget}"
PACKAGE_ID="${KANO_WINGET_PACKAGE_ID:-KanoHorizonia.KanoGit}"
ASSET_BASE_URL="${KANO_RELEASE_ASSET_BASE_URL:-https://github.com/${REPO_SLUG}/releases/download/${TAG_NAME}}"

find_msi() {
  local root found
  for root in "$ARTIFACT_DIR" artifacts artifacts/installers src/wix/out; do
    [ -d "$root" ] || continue
    found="$(find "$root" -type f -iname '*.msi' | sort | head -n 1 || true)"
    if [ -n "$found" ]; then
      printf '%s\n' "$found"
      return 0
    fi
  done
}

calc_sha() {
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
  local ps path_arg
  path_arg="$path"
  if command -v cygpath >/dev/null 2>&1; then
    path_arg="$(cygpath -w "$path")"
  fi
  for ps in pwsh powershell powershell.exe; do
    if command -v "$ps" >/dev/null 2>&1; then
      "$ps" -NoProfile -Command 'param([string]$Path) (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()' "$path_arg" | tr -d '\r'
      return 0
    fi
  done
  echo "ERROR: sha256sum, shasum, openssl, or PowerShell is required to hash $path" >&2
  exit 1
}

MSI_PATH="$(find_msi)"
if [ -z "$MSI_PATH" ]; then
  echo "ERROR: required MSI artifact not found under $ARTIFACT_DIR, artifacts, artifacts/installers, or src/wix/out" >&2
  exit 1
fi

MSI_FILENAME="$(basename "$MSI_PATH")"
MSI_URL="${ASSET_BASE_URL%/}/${MSI_FILENAME}"
MSI_SHA256="$(calc_sha "$MSI_PATH")"

mkdir -p "$OUTPUT_DIR"

cat > "$OUTPUT_DIR/${PACKAGE_ID}.yaml" <<EOF
PackageIdentifier: ${PACKAGE_ID}
PackageVersion: ${VERSION_TEXT}
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.6.0
EOF

cat > "$OUTPUT_DIR/${PACKAGE_ID}.installer.yaml" <<EOF
PackageIdentifier: ${PACKAGE_ID}
PackageVersion: ${VERSION_TEXT}
Installers:
  - Architecture: x64
    InstallerType: wix
    InstallerUrl: ${MSI_URL}
    InstallerSha256: ${MSI_SHA256}
ManifestType: installer
ManifestVersion: 1.6.0
EOF

cat > "$OUTPUT_DIR/${PACKAGE_ID}.locale.en-US.yaml" <<EOF
PackageIdentifier: ${PACKAGE_ID}
PackageVersion: ${VERSION_TEXT}
PackageLocale: en-US
Publisher: Kano Horizonia
PackageName: Kano Git
Moniker: kog
ShortDescription: Kano Git automation toolkit with the KOG CLI and released skill payload.
License: Proprietary
ManifestType: defaultLocale
ManifestVersion: 1.6.0
EOF

echo "$OUTPUT_DIR"
