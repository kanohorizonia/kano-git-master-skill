#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../common/test_report.sh"

: "${KANO_REPORT_SLUG:=windows-x64-test}"
: "${KANO_REPORT_TITLE:=kano-git Windows Test Report}"
export KANO_REPORT_SLUG KANO_REPORT_TITLE
export KANO_TEST_COMMAND='./out/bin/windows-ninja-msvc/release/kano_git_tui_tests.exe --reporter junit --out "$KANO_TEST_XML"'

kog_run_test_report
