#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/kano_cpp_test_skill.sh"

kog_run_test_report() {
  : "${KANO_REPORT_SLUG:?KANO_REPORT_SLUG is required}"
  : "${KANO_TEST_COMMAND:?KANO_TEST_COMMAND is required}"

  kog_export_report_context
  local skill_root report_title
  skill_root="$(kog_require_cpp_test_skill_root)"
  export KANO_CPP_TEST_SKILL_ROOT="$skill_root"
  report_title="${KANO_REPORT_TITLE:-${KANO_REPORT_SLUG}}"

  # shellcheck disable=SC1090
  . "$skill_root/src/shell/reports/common/report-env.sh"

  rm -f "$KANO_TEST_XML"
  mkdir -p "$KANO_TEST_REPORT_DIR"

  (
    cd "$KOG_CPP_ROOT"
    eval "$KANO_TEST_COMMAND"
  )

  python "$SCRIPT_DIR/render_junit_test_report.py" "$KANO_TEST_XML" "$KANO_TEST_REPORT_DIR" "$report_title"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  kog_run_test_report "$@"
fi
