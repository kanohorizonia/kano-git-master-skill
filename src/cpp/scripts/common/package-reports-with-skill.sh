#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/kano_cpp_test_skill.sh"

kog_export_report_context
KANO_CPP_TEST_SKILL_ROOT="$(kog_require_cpp_test_skill_root)"
export KANO_CPP_TEST_SKILL_ROOT
: "${KANO_REPORT_SLUG:=package-all}"
export KANO_REPORT_SLUG

bash "$KANO_CPP_TEST_SKILL_ROOT/src/shell/reports/common/package-reports.sh"
