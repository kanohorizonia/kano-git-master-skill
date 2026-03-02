#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKILL_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
KOG_LAUNCHER="$SKILL_ROOT/scripts/kog"

if [[ ! -x "$KOG_LAUNCHER" ]]; then
  echo "✗ FAIL: missing launcher: $KOG_LAUNCHER" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
cleanup() {
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

WORKSPACE_ROOT="$TMP_DIR/workspace"
ROOT_REMOTE="$TMP_DIR/remote-root.git"
KANO_REMOTE="$TMP_DIR/remote-kano.git"
KANO_REPO="$WORKSPACE_ROOT/.agents/kano"

mkdir -p "$WORKSPACE_ROOT"

git init "$WORKSPACE_ROOT" >/dev/null
git -C "$WORKSPACE_ROOT" config user.name "Test User"
git -C "$WORKSPACE_ROOT" config user.email "test@example.com"
git -C "$WORKSPACE_ROOT" checkout -b main >/dev/null

git init --bare "$ROOT_REMOTE" >/dev/null
git -C "$WORKSPACE_ROOT" remote add origin "$ROOT_REMOTE"
printf "root\n" > "$WORKSPACE_ROOT/README.md"
git -C "$WORKSPACE_ROOT" add README.md
git -C "$WORKSPACE_ROOT" commit -m "init root" >/dev/null
git -C "$WORKSPACE_ROOT" push -u origin main >/dev/null

mkdir -p "$KANO_REPO"
git init "$KANO_REPO" >/dev/null
git -C "$KANO_REPO" config user.name "Test User"
git -C "$KANO_REPO" config user.email "test@example.com"
git -C "$KANO_REPO" checkout -b main >/dev/null

git init --bare "$KANO_REMOTE" >/dev/null
git -C "$KANO_REPO" remote add origin "$KANO_REMOTE"
printf "base\n" > "$KANO_REPO/kano-git-master-skill"
git -C "$KANO_REPO" add kano-git-master-skill
git -C "$KANO_REPO" commit -m "base pointer" >/dev/null
git -C "$KANO_REPO" push -u origin main >/dev/null

base_sha="$(git -C "$KANO_REPO" rev-parse HEAD)"
git -C "$KANO_REPO" checkout -b incoming "$base_sha" >/dev/null
printf "incoming\n" > "$KANO_REPO/kano-git-master-skill"
git -C "$KANO_REPO" add kano-git-master-skill
git -C "$KANO_REPO" commit -m "incoming pointer" >/dev/null
incoming_sha="$(git -C "$KANO_REPO" rev-parse HEAD)"

git -C "$KANO_REPO" checkout main >/dev/null
printf "local\n" > "$KANO_REPO/kano-git-master-skill"
git -C "$KANO_REPO" add kano-git-master-skill
git -C "$KANO_REPO" commit -m "local pointer" >/dev/null

git -C "$KANO_REPO" checkout --detach HEAD >/dev/null
if git -C "$KANO_REPO" cherry-pick "$incoming_sha" >/dev/null 2>&1; then
  echo "✗ FAIL: expected cherry-pick conflict but it succeeded" >&2
  exit 1
fi

if [[ -z "$(git -C "$KANO_REPO" ls-files -u)" ]]; then
  echo "✗ FAIL: expected unmerged index entries in detached HEAD repo" >&2
  exit 1
fi

cat > "$WORKSPACE_ROOT/.gitmodules" <<EOF
[submodule ".agents/kano"]
	path = .agents/kano
	url = $KANO_REMOTE
	branch = main
EOF

git -C "$WORKSPACE_ROOT" add .gitmodules
git -C "$WORKSPACE_ROOT" commit -m "register .agents/kano" >/dev/null

output="$(cd "$WORKSPACE_ROOT" && "$KOG_LAUNCHER" sync pre-commit --branch-mode stable-dev 2>&1)"

if ! grep -Fq "Repo: .agents/kano" <<<"$output"; then
  echo "✗ FAIL: expected .agents/kano recovery log not found" >&2
  echo "$output" >&2
  exit 1
fi

current_branch="$(git -C "$KANO_REPO" symbolic-ref --quiet --short HEAD || true)"
if [[ "$current_branch" != "main" ]]; then
  echo "✗ FAIL: expected .agents/kano to recover to main, got '$current_branch'" >&2
  exit 1
fi

if [[ -n "$(git -C "$KANO_REPO" ls-files -u)" ]]; then
  echo "✗ FAIL: unmerged entries remain after sync pre-commit" >&2
  exit 1
fi

echo "✓ PASS: native pre-commit recovers detached+unmerged submodule by preferring theirs"
