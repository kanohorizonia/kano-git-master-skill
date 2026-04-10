#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
. "$SCRIPT_DIR/../common/kano_cpp_test_skill.sh"

kog_export_report_context
KANO_CPP_TEST_SKILL_ROOT="$(kog_require_cpp_test_skill_root)"
export KANO_CPP_TEST_SKILL_ROOT

: "${KANO_REPORT_SLUG:=windows-x64-coverage}"
export KANO_REPORT_SLUG

to_windows_path() {
  local input="$1"
  if command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$input"
  else
    printf '%s\n' "$input"
  fi
}

coverage_tool="${KOG_WINDOWS_COVERAGE_TOOL:-${KOG_CODE_COVERAGE_CONSOLE:-}}"
if [[ -z "$coverage_tool" ]]; then
  for path in \
    'C:/Program Files/Microsoft Visual Studio/2022/Enterprise/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe' \
    'C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe' \
    'C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe' \
    'C:/Program Files/Microsoft Visual Studio/18/Enterprise/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe' \
    'C:/Program Files/Microsoft Visual Studio/18/Professional/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe' \
    'C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/Extensions/Microsoft/CodeCoverage.Console/Microsoft.CodeCoverage.Console.exe'
  do
    if [[ -x "$path" ]]; then
      coverage_tool="$path"
      break
    fi
  done
fi

[[ -n "$coverage_tool" && -x "$coverage_tool" ]] || {
  echo "[ERROR] Microsoft.CodeCoverage.Console.exe not found." >&2
  exit 1
}

mkdir -p "$KANO_REPORT_ROOT/coverage"
export KANO_COVERAGE_TOOL="$coverage_tool"
runsettings_unix="$KANO_REPORT_ROOT/coverage/windows-coverage.config"
runsettings_win="$(to_windows_path "$runsettings_unix")"
cpp_root_win="$(to_windows_path "$KOG_CPP_ROOT")"
test_binary_unix="$KOG_CPP_ROOT/out/bin/windows-ninja-msvc-coverage/debug/kano_git_tui_tests.exe"
test_binary_win="$(to_windows_path "$test_binary_unix")"
export KANO_RUNSETTINGS="$runsettings_win"

cat > "$runsettings_unix" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<Configuration>
  <CodeCoverage>
    <ModulePaths>
      <Include>
        <ModulePath>.*kano_git_tui_tests\.exe$</ModulePath>
        <ModulePath>.*kano-git\.exe$</ModulePath>
        <ModulePath>.*kano-git-tui\.exe$</ModulePath>
      </Include>
      <IncludeDirectories>
        <Directory Recursive="true">${cpp_root_win}\out\bin\windows-ninja-msvc-coverage\debug</Directory>
      </IncludeDirectories>
    </ModulePaths>
    <SymbolSearchPaths>
      <Path>${cpp_root_win}\out\bin\windows-ninja-msvc-coverage\debug</Path>
      <Path>${cpp_root_win}\out\obj\windows-ninja-msvc-coverage\symbols\debug</Path>
    </SymbolSearchPaths>
    <EnableStaticNativeInstrumentation>True</EnableStaticNativeInstrumentation>
    <EnableDynamicNativeInstrumentation>False</EnableDynamicNativeInstrumentation>
    <EnableStaticNativeInstrumentationRestore>True</EnableStaticNativeInstrumentationRestore>
    <IncludeTestAssembly>True</IncludeTestAssembly>
  </CodeCoverage>
</Configuration>
EOF

export KANO_TEST_COMMAND='./out/bin/windows-ninja-msvc-coverage/debug/kano_git_tui_tests.exe --reporter junit --out "$KANO_TEST_XML" || true'
export KANO_COVERAGE_BINARY_COMMAND="$test_binary_win"
export KANO_COVERAGE_BINARY_ARGS='--reporter junit --out "$KANO_TEST_XML"'

cd "$KOG_CPP_ROOT"
bash "$KANO_CPP_TEST_SKILL_ROOT/src/shell/reports/windows/microsoft-report.sh"
