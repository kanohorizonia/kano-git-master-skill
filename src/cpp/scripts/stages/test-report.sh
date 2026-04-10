#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
INFRA_SCRIPTS_ROOT="$(cd -- "$SCRIPT_DIR/../../shared/infra/build/base/script" && pwd)"
. "$INFRA_SCRIPTS_ROOT/orchestration/matrix.sh"

report_script="$(kog_matrix_default_test_report_script)"
exec bash "$report_script" "$@"
