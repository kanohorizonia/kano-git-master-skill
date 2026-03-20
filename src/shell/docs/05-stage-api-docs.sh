#!/bin/bash
set -euo pipefail

if [ $# -eq 1 ]; then
  BUILD_DIR="$1"
else
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
  BUILD_DIR="$REPO_ROOT/_ws/build"
fi

API_HTML_DIR="$BUILD_DIR/content_api/doxygen-html"
PUBLIC_API_DIR="$BUILD_DIR/public/api-docs"

if [ ! -f "$API_HTML_DIR/index.html" ]; then
  rm -rf "$PUBLIC_API_DIR"
  mkdir -p "$PUBLIC_API_DIR"
  cat > "$PUBLIC_API_DIR/index.html" <<'EOF'
<!DOCTYPE html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>API Docs Unavailable</title>
  </head>
  <body>
    <h1>API docs are unavailable in this build</h1>
    <p>Doxygen output was not generated for this local preview.</p>
    <p>Install <code>doxygen</code> and <code>graphviz</code>, then rerun the docs pipeline to generate C++ API documentation.</p>
    <p><a href="../api/overview.html">Back to API overview</a></p>
  </body>
</html>
EOF
  echo "WARNING: generated API docs not found - staged placeholder page instead"
  exit 0
fi

rm -rf "$PUBLIC_API_DIR"
mkdir -p "$PUBLIC_API_DIR"
cp -R "$API_HTML_DIR/." "$PUBLIC_API_DIR/"

echo "Staged API docs in: $PUBLIC_API_DIR"
