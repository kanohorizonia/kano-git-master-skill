#!/bin/bash
set -euo pipefail

if [ $# -ne 2 ]; then
  echo "Usage: $0 <repo-root> <tag-name>" >&2
  exit 1
fi

REPO_ROOT="$1"
TAG_NAME="$2"
VERSION_TEXT="$(cat "$REPO_ROOT/VERSION")"
EXPECTED_TAG="v${VERSION_TEXT}"

if [ "$TAG_NAME" != "$EXPECTED_TAG" ]; then
  echo "ERROR: tag/version mismatch" >&2
  echo "  tag:      $TAG_NAME" >&2
  echo "  VERSION:  $VERSION_TEXT" >&2
  echo "  expected: $EXPECTED_TAG" >&2
  exit 1
fi

echo "Verified release tag matches VERSION: $TAG_NAME"
