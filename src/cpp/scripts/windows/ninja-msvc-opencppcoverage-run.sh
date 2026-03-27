#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KOG_CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

KOG_COVERAGE_ROOT="${KOG_COVERAGE_ROOT:-${KOG_CPP_ROOT}/out/coverage/opencppcoverage}"
KOG_OPENCPPCOVERAGE_EXE="${KOG_OPENCPPCOVERAGE_EXE:-}"

if [[ -z "$KOG_OPENCPPCOVERAGE_EXE" ]]; then
  for path in \
    "C:/Program Files/OpenCppCoverage/OpenCppCoverage.exe" \
    "C:/tools/OpenCppCoverage/OpenCppCoverage.exe"
  do
    if [[ -x "$path" ]]; then
      KOG_OPENCPPCOVERAGE_EXE="$path"
      break
    fi
  done
fi

if [[ -z "$KOG_OPENCPPCOVERAGE_EXE" || ! -x "$KOG_OPENCPPCOVERAGE_EXE" ]]; then
  echo "[ERROR] OpenCppCoverage.exe not found." >&2
  echo "[ERROR] Set KOG_OPENCPPCOVERAGE_EXE explicitly." >&2
  exit 1
fi

TEST_BINARY="$KOG_CPP_ROOT/out/bin/windows-ninja-msvc-coverage/debug/kano_git_tui_tests.exe"
if [[ ! -f "$TEST_BINARY" ]]; then
  echo "[ERROR] Test binary not found: $TEST_BINARY" >&2
  echo "[ERROR] Run ninja-msvc-coverage-build.sh first." >&2
  exit 1
fi

mkdir -p "$KOG_COVERAGE_ROOT"
HTML_DIR="$KOG_COVERAGE_ROOT/html"
COBERTURA_XML="$KOG_COVERAGE_ROOT/coverage.cobertura.xml"
BINARY_COV="$KOG_COVERAGE_ROOT/opencppcoverage.cov"
TEST_XML="$KOG_COVERAGE_ROOT/tests.xml"
LOG_FILE="$KOG_COVERAGE_ROOT/opencppcoverage.log"
SUMMARY_TXT="$KOG_COVERAGE_ROOT/summary.txt"

to_windows_path() {
  local input="$1"
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$input"
  else
    printf '%s\n' "$input"
  fi
}

CPP_ROOT_WIN="$(to_windows_path "$KOG_CPP_ROOT")"
TEST_BINARY_WIN="$(to_windows_path "$TEST_BINARY")"
HTML_DIR_WIN="$(to_windows_path "$HTML_DIR")"
COBERTURA_XML_WIN="$(to_windows_path "$COBERTURA_XML")"
BINARY_COV_WIN="$(to_windows_path "$BINARY_COV")"
TEST_XML_WIN="$(to_windows_path "$TEST_XML")"

rm -rf "$HTML_DIR"
mkdir -p "$HTML_DIR"

echo "[opencppcoverage] Tool:   $KOG_OPENCPPCOVERAGE_EXE"
echo "[opencppcoverage] Binary: $TEST_BINARY_WIN"
echo "[opencppcoverage] Output: $KOG_COVERAGE_ROOT"

"$KOG_OPENCPPCOVERAGE_EXE" \
  --modules "*kano*" \
  --sources "$CPP_ROOT_WIN\code" \
  --excluded_sources "$CPP_ROOT_WIN\out\obj\*" \
  --excluded_sources "$CPP_ROOT_WIN\out\bin\*" \
  --export_type="html:$HTML_DIR_WIN" \
  --export_type="cobertura:$COBERTURA_XML_WIN" \
  --export_type="binary:$BINARY_COV_WIN" \
  -- "$TEST_BINARY_WIN" --reporter junit --out "$TEST_XML_WIN" \
  2>&1 | tee "$LOG_FILE"

python - <<'PY' "$COBERTURA_XML" "$SUMMARY_TXT"
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

xml_path = Path(sys.argv[1])
summary_path = Path(sys.argv[2])
root = ET.fromstring(xml_path.read_text(encoding='utf-8-sig'))
line_rate = float(root.get('line-rate', '0')) * 100
branch_rate = float(root.get('branch-rate', '0')) * 100
lines_covered = root.get('lines-covered', '0')
lines_valid = root.get('lines-valid', '0')
summary_path.write_text(
    f"Line coverage: {line_rate:.2f}%\n"
    f"Branch coverage: {branch_rate:.2f}%\n"
    f"Lines covered: {lines_covered}\n"
    f"Lines valid: {lines_valid}\n",
    encoding='utf-8',
)
PY

echo "[opencppcoverage] HTML:      $HTML_DIR/index.html"
echo "[opencppcoverage] Cobertura: $COBERTURA_XML"
echo "[opencppcoverage] Binary:    $BINARY_COV"
echo "[opencppcoverage] JUnit:     $TEST_XML"
echo "[opencppcoverage] Summary:   $SUMMARY_TXT"
