#!/bin/bash
set -euo pipefail

if [ $# -ne 5 ]; then
  echo "Usage: $0 <repo-root> <tag-name> <repo-slug> <artifact-dir> <output-file>" >&2
  exit 1
fi

REPO_ROOT="$1"
TAG_NAME="$2"
REPO_SLUG="$3"
ARTIFACT_DIR="$4"
OUTPUT_FILE="$5"

VERSION_TEXT="$(cat "$REPO_ROOT/VERSION")"

{
  echo "# ${TAG_NAME}"
  echo
  echo "Canonical version: ${VERSION_TEXT}"
  echo "GitHub release page: https://github.com/${REPO_SLUG}/releases/tag/${TAG_NAME}"
  echo
  echo "## Included artifacts"
  find "$ARTIFACT_DIR" -maxdepth 2 -type f | sort | sed 's#^#- `#; s#$#`#'
  echo
  echo "## Distribution status"
  echo "- GitHub Release publishing is active in this pipeline."
  echo "- winget/Homebrew outputs are generated as preview skeletons only."
} > "$OUTPUT_FILE"

echo "$OUTPUT_FILE"
