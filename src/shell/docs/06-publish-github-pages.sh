#!/bin/bash
set -euo pipefail

SITE_OUTPUT="${1:-${KANO_SITE_OUTPUT_ROOT:-site-dist}}"
REPO_URL="${KANO_GITHUB_PAGES_REPO_URL:-}"
TARGET_BRANCH="${KANO_GITHUB_PAGES_BRANCH:-gh-pages}"
COMMIT_MESSAGE="${KANO_GITHUB_PAGES_COMMIT_MESSAGE:-Publish ${KANO_PROJECT_NAME:-site} ${KANO_RELEASE_TAG:-release}}"

if [ -z "$REPO_URL" ]; then
  echo "ERROR: KANO_GITHUB_PAGES_REPO_URL is required for GitHub Pages publish" >&2
  exit 1
fi
if [ ! -d "$SITE_OUTPUT" ]; then
  echo "ERROR: site output directory not found: $SITE_OUTPUT" >&2
  exit 1
fi

SITE_OUTPUT_ABS="$(cd "$SITE_OUTPUT" && pwd)"
tmp_root="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp_root"
}
trap cleanup EXIT

clone_url="$REPO_URL"
if [ -n "${KANO_GITHUB_PAGES_REPO_USER:-}" ] && [ -n "${KANO_GITHUB_PAGES_REPO_TOKEN:-}" ] && [[ "$REPO_URL" == https://* ]]; then
  clone_url="https://${KANO_GITHUB_PAGES_REPO_USER}:${KANO_GITHUB_PAGES_REPO_TOKEN}@${REPO_URL#https://}"
fi

if git ls-remote --exit-code --heads "$clone_url" "$TARGET_BRANCH" >/dev/null 2>&1; then
  git clone --depth=1 --branch "$TARGET_BRANCH" "$clone_url" "$tmp_root/repo" >/dev/null
else
  git clone "$clone_url" "$tmp_root/repo" >/dev/null
  cd "$tmp_root/repo"
  git checkout --orphan "$TARGET_BRANCH" >/dev/null
  git rm -rf . >/dev/null 2>&1 || true
fi

cd "$tmp_root/repo"
find . -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp -R "$SITE_OUTPUT_ABS"/. .
touch .nojekyll

git add -A
if git diff --cached --quiet; then
  echo "No GitHub Pages changes to publish."
  exit 0
fi

git -c user.name="${KANO_GITHUB_PAGES_GIT_USER_NAME:-Kano Jenkins}" \
    -c user.email="${KANO_GITHUB_PAGES_GIT_USER_EMAIL:-jenkins@kanohorizonia.local}" \
    commit -m "$COMMIT_MESSAGE" >/dev/null
git push origin "HEAD:${TARGET_BRANCH}" >/dev/null
echo "Published GitHub Pages payload to ${TARGET_BRANCH}."
