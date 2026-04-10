#!/usr/bin/env bash
# package-reports.sh — delegates to kano-cpp-test-skill adapter for report packaging
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
COMMON_SCRIPTS="$(cd -- "$SCRIPT_DIR/../common" && pwd)"
exec bash "$COMMON_SCRIPTS/package-reports-with-skill.sh" "$@"
