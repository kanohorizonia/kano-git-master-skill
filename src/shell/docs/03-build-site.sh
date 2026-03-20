#!/bin/bash
set -euo pipefail

if [ $# -eq 4 ]; then
  REPO_ROOT="$1"
  QUARTZ_DIR="$2"
  BUILD_DIR="$3"
  CONFIG_FILE="$4"
else
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
  QUARTZ_DIR="$REPO_ROOT/_ws/src/quartz"
  BUILD_DIR="$REPO_ROOT/_ws/build"
  CONFIG_FILE="$SCRIPT_DIR/config/quartz.config.template.txt"
fi

CONTENT_DIR="$BUILD_DIR/content_quartz"
OUTPUT_DIR="$BUILD_DIR/public"

mkdir -p "$OUTPUT_DIR"
find "$OUTPUT_DIR" -mindepth 1 -maxdepth 1 -exec rm -rf {} +

cd "$QUARTZ_DIR"
npm ci
cp "$CONFIG_FILE" ./quartz.config.ts
npx quartz build --directory "$CONTENT_DIR" --output "$OUTPUT_DIR"

echo "Built site in: $OUTPUT_DIR"
