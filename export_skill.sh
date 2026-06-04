#!/usr/bin/env bash
set -euo pipefail

# Export workspace using kog export.

PROJECT_ROOT="${1:-$(pwd)}"
OUTPUT_DIR="${2:-$PROJECT_ROOT/_exported_project}"

PROJECT_ROOT="$(cd "$PROJECT_ROOT" && pwd -W)"
OUTPUT_DIR="$(mkdir -p "$OUTPUT_DIR" && cd "$OUTPUT_DIR" && pwd -W)"

cd "$PROJECT_ROOT"
kog export --single --no-validate-release-archive --output "$OUTPUT_DIR"

echo "Export complete"
