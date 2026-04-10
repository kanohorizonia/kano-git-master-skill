#!/usr/bin/env bash
# package-reports-with-skill.sh — delegates to external test skill for report packaging
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/report_skill_adapter.sh"

report_skill_load
report_skill_package "$@"
