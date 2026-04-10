#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../orchestration/matrix.sh"

backend="${1:-default}"
if [[ "$#" -gt 0 ]]; then
  shift
fi

coverage_script="$(kog_matrix_default_coverage_report_script "$backend")"
exec bash "$coverage_script" "$@"
