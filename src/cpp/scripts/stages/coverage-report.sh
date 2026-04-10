#!/usr/bin/env bash
# coverage-report.sh — delegates to kano-cpp-infra matrix.sh for platform routing
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INFRA_SCRIPTS_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/build/base/script" && pwd)"
. "$INFRA_SCRIPTS_ROOT/orchestration/matrix.sh"

backend="${1:-default}"
if [[ "$#" -gt 0 ]]; then
  shift
fi

coverage_script="$(kog_matrix_default_coverage_report_script "$backend")"
exec bash "$coverage_script" "$@"
