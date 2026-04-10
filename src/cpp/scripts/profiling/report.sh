#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/common.sh"

MATRIX_NAME="${1:-default}"
MATRIX_PATH="$(kog_profile_require_matrix "$MATRIX_NAME")"

python "$SCRIPT_DIR/render_profile_report.py" "$MATRIX_PATH" "$KOG_PROFILE_TMP_ROOT" "$KOG_PROFILE_REPORT_ROOT"
