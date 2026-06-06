#!/bin/bash
set -euo pipefail

SOURCE_DIR="${1:-${KANO_PACKAGE_MANAGER_RECIPE_ROOT:-Release/package-managers}/homebrew}"
REPO_URL="${KANO_HOMEBREW_TAP_REPO_URL:-}"
TARGET_BRANCH="${KANO_HOMEBREW_TARGET_BRANCH:-main}"
FORMULA_NAME="${KANO_HOMEBREW_FORMULA_NAME:-kano-git-master-skill}"
VERSION_TEXT="$(tr -d '[:space:]' < VERSION)"

if [ -z "$REPO_URL" ]; then
  echo "ERROR: KANO_HOMEBREW_TAP_REPO_URL is required for internal Homebrew publish" >&2
  exit 1
fi
if [ ! -d "$SOURCE_DIR" ]; then
  echo "ERROR: Homebrew formula source directory not found: $SOURCE_DIR" >&2
  exit 1
fi
SOURCE_DIR_ABS="$(cd "$SOURCE_DIR" && pwd)"

formula_source="$SOURCE_DIR_ABS/${FORMULA_NAME}.rb"
if [ ! -f "$formula_source" ]; then
  echo "ERROR: Homebrew formula not found: $formula_source" >&2
  exit 1
fi

tmp_root="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp_root"
}
trap cleanup EXIT

clone_url="$REPO_URL"
if [ -n "${KANO_HOMEBREW_REPO_USER:-}" ] && [ -n "${KANO_HOMEBREW_REPO_TOKEN:-}" ] && [[ "$REPO_URL" == https://* ]]; then
  clone_url="https://${KANO_HOMEBREW_REPO_USER}:${KANO_HOMEBREW_REPO_TOKEN}@${REPO_URL#https://}"
fi

git clone "$clone_url" "$tmp_root/repo" >/dev/null
cd "$tmp_root/repo"
git checkout "$TARGET_BRANCH" >/dev/null 2>&1 || git checkout -b "$TARGET_BRANCH" >/dev/null

mkdir -p Formula
cp -f "$formula_source" "Formula/${FORMULA_NAME}.rb"
git add "Formula/${FORMULA_NAME}.rb"
if git diff --cached --quiet; then
  echo "No Homebrew formula changes to publish."
  exit 0
fi

git -c user.name="${KANO_PACKAGE_MANAGER_GIT_USER_NAME:-Kano Jenkins}" \
    -c user.email="${KANO_PACKAGE_MANAGER_GIT_USER_EMAIL:-jenkins@kanohorizonia.local}" \
    commit -m "Update ${FORMULA_NAME} ${VERSION_TEXT}" >/dev/null
git push origin "HEAD:${TARGET_BRANCH}" >/dev/null
echo "Published Homebrew formula to ${TARGET_BRANCH}: Formula/${FORMULA_NAME}.rb"
