#!/bin/bash
set -euo pipefail

if [ $# -eq 3 ]; then
  REPO_ROOT="$1"
  RAW_DIR="$2"
  BUILD_DIR="$3"
else
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
  RAW_DIR="$REPO_ROOT/_site/src/raw"
  BUILD_DIR="$REPO_ROOT/_site/build"
fi

API_DIR="$BUILD_DIR/content_api"
HTML_DIR="$API_DIR/doxygen-html"
CONFIG_TEMPLATE="$REPO_ROOT/src/shell/docs/config/Doxyfile.in"
TEMP_CONFIG="$API_DIR/Doxyfile"

rm -rf "$API_DIR"
mkdir -p "$HTML_DIR"

if ! command -v doxygen >/dev/null 2>&1; then
  echo "WARNING: doxygen not found - skipping C++ API docs generation"
  echo "Install doxygen and graphviz locally to generate API docs in local previews"
  exit 0
fi

RAW_DIR_POSIX="$(cd "$RAW_DIR" && pwd)"
HTML_DIR_POSIX="$(cd "$HTML_DIR" && pwd)"

python - <<'PY' "$CONFIG_TEMPLATE" "$TEMP_CONFIG" "$RAW_DIR_POSIX" "$HTML_DIR_POSIX"
from pathlib import Path
import sys

template_path = Path(sys.argv[1])
output_path = Path(sys.argv[2])
raw_dir = sys.argv[3]
html_dir = sys.argv[4]

content = template_path.read_text(encoding="utf-8")
content = content.replace("@RAW_DIR@", raw_dir.replace('\\', '/'))
content = content.replace("@OUTPUT_DIR@", html_dir.replace('\\', '/'))
output_path.write_text(content, encoding="utf-8")
PY

doxygen "$TEMP_CONFIG"
echo "Built Doxygen API docs in: $HTML_DIR"
