#!/bin/bash
set -euo pipefail

if [ $# -ne 5 ]; then
  echo "Usage: $0 <repo-root> <tag-name> <repo-slug> <artifact-dir> <output-dir>" >&2
  exit 1
fi

REPO_ROOT="$1"
TAG_NAME="$2"
REPO_SLUG="$3"
ARTIFACT_DIR="$4"
OUTPUT_DIR="$5"

VERSION_TEXT="$(cat "$REPO_ROOT/VERSION")"
PACKAGE_ID="Kanohorizonia.KanoGitMasterSkill"
MSI_PATH="$(find "$ARTIFACT_DIR" -type f -name '*.msi' | sort | head -n 1 || true)"
MSI_SHA256=""
MSI_URL="REPLACE_WITH_GITHUB_RELEASE_MSI_URL"

mkdir -p "$OUTPUT_DIR"

if [ -z "$MSI_PATH" ]; then
  echo "ERROR: required MSI artifact not found under $ARTIFACT_DIR" >&2
  exit 1
fi

MSI_FILENAME="$(basename "$MSI_PATH")"
MSI_URL="https://github.com/${REPO_SLUG}/releases/download/${TAG_NAME}/${MSI_FILENAME}"
MSI_SHA256="$(python - <<'PY' "$MSI_PATH"
from pathlib import Path
import hashlib, sys
path = Path(sys.argv[1])
print(hashlib.sha256(path.read_bytes()).hexdigest())
PY
)"

cat > "$OUTPUT_DIR/${PACKAGE_ID}.yaml" <<EOF
PackageIdentifier: ${PACKAGE_ID}
PackageVersion: ${VERSION_TEXT}
PackageLocale: en-US
Publisher: Kanohorizonia
PackageName: kano-git-master-skill
ShortDescription: Kano Git Master Skill packaged release preview
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
