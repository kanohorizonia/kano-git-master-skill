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
PACKAGE_ID="${KANO_WINGET_PACKAGE_ID:-Kanohorizonia.KanoGitMasterSkill}"
ASSET_BASE_URL="${KANO_RELEASE_ASSET_BASE_URL:-https://github.com/${REPO_SLUG}/releases/download/${TAG_NAME}}"

find_msi() {
  local root
  for root in "$ARTIFACT_DIR" artifacts/installers src/wix/out; do
    [ -d "$root" ] || continue
    find "$root" -type f -name '*.msi' | sort | head -n 1
  done | awk 'NF { print; exit }'
}

calc_sha() {
  python - "$1" <<'PY'
from pathlib import Path
import hashlib
import sys
path = Path(sys.argv[1])
print(hashlib.sha256(path.read_bytes()).hexdigest())
PY
}

MSI_PATH="$(find_msi)"
if [ -z "$MSI_PATH" ]; then
  echo "ERROR: required MSI artifact not found under $ARTIFACT_DIR, artifacts/installers, or src/wix/out" >&2
  exit 1
fi

MSI_FILENAME="$(basename "$MSI_PATH")"
MSI_URL="${ASSET_BASE_URL%/}/${MSI_FILENAME}"
MSI_SHA256="$(calc_sha "$MSI_PATH")"

mkdir -p "$OUTPUT_DIR"

cat > "$OUTPUT_DIR/${PACKAGE_ID}.yaml" <<EOF
PackageIdentifier: ${PACKAGE_ID}
PackageVersion: ${VERSION_TEXT}
PackageLocale: en-US
Publisher: Kanohorizonia
PackageName: kano-git-master-skill
ShortDescription: Kano Git Master Skill
License: Proprietary
ManifestType: defaultLocale
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

cat > "$OUTPUT_DIR/${PACKAGE_ID}.version.yaml" <<EOF
PackageIdentifier: ${PACKAGE_ID}
PackageVersion: ${VERSION_TEXT}
ManifestType: version
ManifestVersion: 1.6.0
EOF

echo "$OUTPUT_DIR"
