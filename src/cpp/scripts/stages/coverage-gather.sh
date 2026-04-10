#!/usr/bin/env bash
# coverage-gather.sh — delegates to kano-cpp-infra matrix.sh for platform routing
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INFRA_SCRIPTS_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/scripts" && pwd)"
. "$INFRA_SCRIPTS_ROOT/orchestration/matrix.sh"

coverage_script="$(kog_matrix_default_coverage_gather_script)"
exec bash "$coverage_script" "$@"
