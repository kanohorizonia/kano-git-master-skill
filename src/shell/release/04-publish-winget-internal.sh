#!/bin/bash
set -euo pipefail

SOURCE_DIR="${1:-${KANO_PACKAGE_MANAGER_RECIPE_ROOT:-Release/package-managers}/winget}"
REPO_URL="${KANO_WINGET_MANIFEST_REPO_URL:-}"
TARGET_BRANCH="${KANO_WINGET_TARGET_BRANCH:-main}"
PACKAGE_ID="${KANO_WINGET_PACKAGE_ID:-Kanohorizonia.KanoGitMasterSkill}"
VERSION_TEXT="$(tr -d '[:space:]' < VERSION)"

if [ -z "$REPO_URL" ]; then
  echo "ERROR: KANO_WINGET_MANIFEST_REPO_URL is required for internal winget publish" >&2
  exit 1
fi
if [ ! -d "$SOURCE_DIR" ]; then
  echo "ERROR: winget recipe source directory not found: $SOURCE_DIR" >&2
  exit 1
fi
SOURCE_DIR_ABS="$(cd "$SOURCE_DIR" && pwd)"

tmp_root="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp_root"
}
trap cleanup EXIT

clone_url="$REPO_URL"
if [ -n "${KANO_WINGET_REPO_USER:-}" ] && [ -n "${KANO_WINGET_REPO_TOKEN:-}" ] && [[ "$REPO_URL" == https://* ]]; then
  clone_url="https://${KANO_WINGET_REPO_USER}:${KANO_WINGET_REPO_TOKEN}@${REPO_URL#https://}"
fi

git clone "$clone_url" "$tmp_root/repo" >/dev/null
cd "$tmp_root/repo"
git checkout "$TARGET_BRANCH" >/dev/null 2>&1 || git checkout -b "$TARGET_BRANCH" >/dev/null

package_path="manifests/$(printf '%s' "$PACKAGE_ID" | cut -c1 | tr '[:upper:]' '[:lower:]')/$(printf '%s' "$PACKAGE_ID" | tr '.' '/')/$VERSION_TEXT"
mkdir -p "$package_path"
cp -f "$SOURCE_DIR_ABS"/*.yaml "$package_path/"

git add "$package_path"
if git diff --cached --quiet; then
  echo "No winget manifest changes to publish."
  exit 0
fi

git -c user.name="${KANO_PACKAGE_MANAGER_GIT_USER_NAME:-Kano Jenkins}" \
    -c user.email="${KANO_PACKAGE_MANAGER_GIT_USER_EMAIL:-jenkins@kanohorizonia.local}" \
    commit -m "Update ${PACKAGE_ID} ${VERSION_TEXT}" >/dev/null
git push origin "HEAD:${TARGET_BRANCH}" >/dev/null
echo "Published winget manifests to ${TARGET_BRANCH}: ${package_path}"
