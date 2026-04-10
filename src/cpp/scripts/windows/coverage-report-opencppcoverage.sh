#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../common/kano_cpp_test_skill.sh"

kog_export_report_context
KANO_CPP_TEST_SKILL_ROOT="$(kog_require_cpp_test_skill_root)"
export KANO_CPP_TEST_SKILL_ROOT

: "${KANO_REPORT_SLUG:=windows-x64-opencppcoverage}"
export KANO_REPORT_SLUG

tool="${KOG_OPENCPPCOVERAGE_EXE:-${KANO_OPENCPPCOVERAGE_EXE:-}}"
if [[ -z "$tool" ]]; then
  for path in 'C:/Program Files/OpenCppCoverage/OpenCppCoverage.exe' 'C:/tools/OpenCppCoverage/OpenCppCoverage.exe'; do
    if [[ -x "$path" ]]; then
      tool="$path"
      break
    fi
  done
fi

[[ -n "$tool" && -x "$tool" ]] || {
  echo "[ERROR] OpenCppCoverage.exe not found." >&2
  exit 1
}

export KANO_OPENCPPCOVERAGE_EXE="$tool"
export KANO_OPENCPPCOVERAGE_SOURCES="$KOG_CPP_ROOT/code"
unset KANO_OPENCPPCOVERAGE_MODULES
export KANO_OPENCPPCOVERAGE_TEST_COMMAND='./out/bin/windows-ninja-msvc-coverage/debug/kano_git_tui_tests.exe --reporter junit --out "$KANO_TEST_XML" || true'

cd "$KOG_CPP_ROOT"
bash "$KANO_CPP_TEST_SKILL_ROOT/src/shell/reports/windows/opencppcoverage-report.sh"
