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

CONTENT_DIR="$BUILD_DIR/content_quartz"
rm -rf "$CONTENT_DIR"
mkdir -p "$CONTENT_DIR"

cp -R "$RAW_DIR/docs/." "$CONTENT_DIR/"
cp "$RAW_DIR/docs/README.md" "$CONTENT_DIR/index.md"

mkdir -p "$CONTENT_DIR/api"
cat > "$CONTENT_DIR/api/overview.md" <<'EOF'
---
title: API Overview
---

# API Overview

This site publishes generated C++ API documentation under `/api-docs/`.

- [Open generated API docs](/api-docs/)

The generated API pages are produced from the public C++ facade headers with Doxygen.
EOF

cat > "$CONTENT_DIR/_index.md" <<'EOF'
---
title: Home
---
EOF

echo "Prepared content in: $CONTENT_DIR"
