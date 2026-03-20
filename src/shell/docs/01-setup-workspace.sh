#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
CONFIG_FILE="$SCRIPT_DIR/config/build.json"

QUARTZ_VERSION=$(python -c "import json,sys;print(json.load(open(sys.argv[1], encoding='utf-8'))['quartz']['version'])" "$CONFIG_FILE")
QUARTZ_REPO=$(python -c "import json,sys;print(json.load(open(sys.argv[1], encoding='utf-8'))['quartz']['repository'])" "$CONFIG_FILE")

mkdir -p "$REPO_ROOT/_ws/src" "$REPO_ROOT/_ws/build" "$REPO_ROOT/_ws/deploy/gh-pages"

if [ ! -d "$REPO_ROOT/_ws/src/raw/.git" ]; then
  git clone "$REPO_ROOT" "$REPO_ROOT/_ws/src/raw"
else
  git -C "$REPO_ROOT/_ws/src/raw" fetch origin >/dev/null 2>&1 || true
fi

if [ ! -d "$REPO_ROOT/_ws/src/quartz/.git" ]; then
  git clone --branch "$QUARTZ_VERSION" --depth 1 "$QUARTZ_REPO" "$REPO_ROOT/_ws/src/quartz"
else
  git -C "$REPO_ROOT/_ws/src/quartz" fetch origin --tags >/dev/null 2>&1 || true
  git -C "$REPO_ROOT/_ws/src/quartz" checkout "$QUARTZ_VERSION"
fi

mkdir -p "$REPO_ROOT/_ws/build/content_quartz" "$REPO_ROOT/_ws/build/content_api" "$REPO_ROOT/_ws/build/public"
echo "Workspace ready: $REPO_ROOT/_ws"
